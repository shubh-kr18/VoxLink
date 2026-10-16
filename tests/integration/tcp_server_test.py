
import socket
import subprocess
import time
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SERVER = ROOT / "build" / "VoxLink_server"
HOST = "127.0.0.1"
PORT = 5050


class TcpServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = subprocess.Popen(
            [str(SERVER)],
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        deadline = time.time() + 5

        while time.time() < deadline:
            if cls.server.poll() is not None:
                raise RuntimeError("VoxLink server exited unexpectedly")

            try:
                with socket.create_connection(
                    (HOST, PORT), timeout=0.2
                ):
                    break
            except OSError:
                time.sleep(0.05)
        else:
            cls.server.terminate()
            cls.server.wait(timeout=2)
            raise RuntimeError("VoxLink server did not start")

    @classmethod
    def tearDownClass(cls):
        cls.server.terminate()

        try:
            cls.server.wait(timeout=3)
        except subprocess.TimeoutExpired:
            cls.server.kill()
            cls.server.wait()

    def setUp(self):
        self.client = socket.create_connection(
            (HOST, PORT), timeout=2
        )
        self.reader = self.client.makefile("r", encoding="utf-8")

    def tearDown(self):
        self.reader.close()
        self.client.close()

    def command(self, text):
        self.client.sendall((text + "\n").encode())
        return self.reader.readline().strip()

    def test_floor_control_flow(self):
        self.assertEqual(
            self.command("ACQUIRE integration alice"),
            "GRANTED 1",
        )

        self.assertEqual(
            self.command("ACQUIRE integration bob"),
            "DENIED",
        )

        self.assertEqual(
            self.command("OWNER integration"),
            "OWNER alice",
        )

        self.assertEqual(
            self.command("RELEASE integration alice 1"),
            "RELEASED",
        )

        self.assertEqual(
            self.command("ACQUIRE integration bob"),
            "GRANTED 2",
        )

        self.assertEqual(
            self.command("OWNER integration"),
            "OWNER bob",
        )

    def test_multiple_clients_are_handled_concurrently(self):
        # Alice acquires the floor and keeps her connection open.
        response = self.command("ACQUIRE concurrent alice")
        self.assertTrue(response.startswith("GRANTED "))

        bob = socket.create_connection((HOST, PORT), timeout=2)
        bob_reader = bob.makefile("r", encoding="utf-8")

        try:
            # Bob must get a response while Alice is still connected.
            bob.sendall(b"OWNER concurrent\n")
            self.assertEqual(
                bob_reader.readline().strip(),
                "OWNER alice",
            )

            bob.sendall(b"ACQUIRE concurrent bob\n")
            self.assertEqual(
                bob_reader.readline().strip(),
                "DENIED",
            )
        finally:
            bob_reader.close()
            bob.close()
    def test_rejects_clients_beyond_limit(self):
        held_clients = []

        try:
            # The setUp() method already opened one client.
            # Open 31 more to fill all 32 slots.
            for _ in range(31):
                client = socket.create_connection(
                    (HOST, PORT), timeout=2
                )
                held_clients.append(client)

            time.sleep(0.1)

            # The 33rd connection should be rejected.
            with socket.create_connection(
                (HOST, PORT), timeout=2
            ) as overflow:
                overflow.settimeout(2)
                response = b""

                while not response.endswith(b"\n"):
                    chunk = overflow.recv(128)

                    if not chunk:
                        break

                    response += chunk

            self.assertEqual(
                response.decode().strip(),
                "ERR server_busy",
            )

        finally:
            for client in held_clients:
                client.close()


if __name__ == "__main__":
    unittest.main()
