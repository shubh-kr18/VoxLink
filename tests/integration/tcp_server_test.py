
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
    def start_server(cls):
        server = subprocess.Popen(
            [str(SERVER)],
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        deadline = time.time() + 5

        while time.time() < deadline:
            if server.poll() is not None:
                raise RuntimeError(
                    "VoxLink server exited unexpectedly"
                )

            try:
                with socket.create_connection(
                    (HOST, PORT), timeout=0.2
                ):
                    return server
            except OSError:
                time.sleep(0.05)

        server.terminate()

        try:
            server.wait(timeout=3)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()

        raise RuntimeError("VoxLink server did not start")

    @classmethod
    def setUpClass(cls):
        cls.server = cls.start_server()

    @classmethod
    def tearDownClass(cls):
        if cls.server.poll() is None:
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
        self.reader = self.client.makefile(
            "r", encoding="utf-8"
        )

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
        response = self.command("ACQUIRE concurrent alice")
        self.assertTrue(response.startswith("GRANTED "))

        bob = socket.create_connection(
            (HOST, PORT), timeout=2
        )
        bob_reader = bob.makefile(
            "r", encoding="utf-8"
        )

        try:
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
            # setUp() already occupies one client slot.
            for _ in range(31):
                held_clients.append(
                    socket.create_connection(
                        (HOST, PORT), timeout=2
                    )
                )

            time.sleep(0.1)

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

    def test_rejects_malformed_commands(self):
        invalid_commands = [
            ("ACQUIRE", "ERR invalid_command"),
            ("ACQUIRE channel", "ERR invalid_command"),
            ("ACQUIRE channel alice extra", "ERR invalid_command"),
            ("OWNER", "ERR invalid_command"),
            ("OWNER channel extra", "ERR invalid_command"),
            ("RELEASE channel alice nope", "ERR invalid_command"),
            ("RELEASE channel alice 1 extra", "ERR invalid_command"),
            ("RELEASE channel alice -1", "ERR invalid_command"),
            (
                "RELEASE channel alice 18446744073709551616",
                "ERR invalid_command",
            ),
            ("UNKNOWN channel", "ERR unknown_command"),
        ]

        for command, expected in invalid_commands:
            with self.subTest(command=command):
                self.assertEqual(
                    self.command(command),
                    expected,
                )

    def test_rejects_oversized_line(self):
        self.client.sendall(b"x" * 1025 + b"\n")

        self.assertEqual(
            self.reader.readline().strip(),
            "ERR line_too_long",
        )

    def test_rapid_connect_disconnect(self):
        for _ in range(40):
            with socket.create_connection(
                (HOST, PORT), timeout=2
            ):
                pass

        self.assertEqual(
            self.command("OWNER reconnect"),
            "OWNER NONE",
        )

    def test_shutdown_and_restart(self):
        idle_client = socket.create_connection(
            (HOST, PORT), timeout=2
        )

        try:
            self.__class__.server.terminate()
            self.__class__.server.wait(timeout=3)

            self.assertEqual(
                self.__class__.server.returncode,
                0,
                "Server did not exit cleanly",
            )

            idle_client.settimeout(2)
            self.assertEqual(idle_client.recv(1), b"")
        finally:
            idle_client.close()

        self.__class__.server = self.start_server()

        with socket.create_connection(
            (HOST, PORT), timeout=2
        ) as client:
            client.settimeout(2)
            client.sendall(b"OWNER restarted\n")

            response = b""

            while not response.endswith(b"\n"):
                chunk = client.recv(128)

                if not chunk:
                    break

                response += chunk

            self.assertEqual(
                response.decode().strip(),
                "OWNER NONE",
            )


if __name__ == "__main__":
    unittest.main()
