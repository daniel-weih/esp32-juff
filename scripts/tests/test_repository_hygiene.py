from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]


class RepositoryHygieneTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / "scripts").mkdir()
        for name in ("check_repository.sh", "check_repository.py"):
            shutil.copy2(REPO / "scripts" / name, self.root / "scripts" / name)
        for name in ("LICENSE", "README.md", "CONTRIBUTING.md", "SECURITY.md",
                     "host/.env.example", "host/package-lock.json"):
            path = self.root / name
            path.parent.mkdir(exist_ok=True)
            path.write_text("Public fixture\n")
        (self.root / ".gitignore").write_text(".env\nbackups/\ntmp/\n")
        self.git("init", "-q")
        self.git("add", ".")

    def git(self, *arguments):
        return subprocess.run(["git", *arguments], cwd=self.root, check=True,
                              capture_output=True)

    def run_check(self):
        return subprocess.run(["sh", "scripts/check_repository.sh"], cwd=self.root,
                              capture_output=True, text=True)

    def test_untracked_files_with_spaces_are_scanned_and_values_are_redacted(self):
        path = self.root / "new notes.md"
        secret = "sk-" + "a" * 32
        personal_path = "/" + "Users" + "/sample-owner/project"
        path.write_text(secret + "\n" + personal_path + "\n")
        result = self.run_check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("new notes.md", result.stderr)
        self.assertIn("API credential", result.stderr)
        self.assertIn("machine-specific absolute path", result.stderr)
        self.assertNotIn(secret, result.stdout + result.stderr)
        self.assertNotIn(personal_path, result.stdout + result.stderr)

    def test_index_secret_is_detected_after_working_copy_is_cleaned_or_deleted(self):
        path = self.root / "settings.md"
        secret = "github_pat_" + "b" * 40
        path.write_text(secret)
        self.git("add", "settings.md")
        for deleted in (False, True):
            with self.subTest(deleted=deleted):
                if deleted:
                    path.unlink()
                else:
                    path.write_text("Clean working copy")
                result = self.run_check()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("(index): API credential", result.stderr)
                self.assertNotIn(secret, result.stdout + result.stderr)

    def test_untracked_hardware_device_ids_report_location_without_value(self):
        # Deliberately synthetic; construct the identifier so this test source
        # does not itself look like a published device identifier.
        device_id = "esp32s3-" + "0123456789ab"
        for value in (device_id, device_id.upper()):
            with self.subTest(uppercase=value.isupper()):
                (self.root / "device example.md").write_text(
                    'Protocol example\n{"deviceId": "' + value + '"}\n')
                result = self.run_check()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('"device example.md":2 (working tree): hardware-derived device ID',
                              result.stderr)
                self.assertNotIn(value, result.stdout + result.stderr)
                self.assertNotIn(value.split("-")[1], result.stdout + result.stderr)

    def test_index_device_id_survives_cleaned_or_deleted_working_copy(self):
        path = self.root / "device example.md"
        device_id = "esp32s3-" + "0123456789ab"
        path.write_text('Protocol example\n{"deviceId": "' + device_id + '"}\n')
        self.git("add", "device example.md")
        for deleted in (False, True):
            with self.subTest(deleted=deleted):
                if deleted:
                    path.unlink()
                else:
                    path.write_text('{"deviceId": "esp32s3-XXXXXXXXXXXX"}\n')
                result = self.run_check()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('"device example.md":2 (index): hardware-derived device ID',
                              result.stderr)
                self.assertNotIn("(working tree): hardware-derived device ID", result.stderr)
                self.assertNotIn(device_id, result.stdout + result.stderr)
                self.assertNotIn(device_id.split("-")[1], result.stdout + result.stderr)

    def test_device_placeholders_ble_uuids_and_other_lengths_are_allowed(self):
        prefix = "esp32s3-"
        public_examples = (prefix + "XXXXXXXXXXXX", prefix + "<device-id>",
                           prefix + "0123456789a", prefix + "0123456789abc",
                           "E6F9A001-7B3C-4D5E-9F01-23456789ABCD")
        (self.root / "public examples.md").write_text("\n".join(public_examples))
        self.git("add", "public examples.md")
        # Different working content forces the checker to inspect the index too.
        (self.root / "public examples.md").write_text("Public examples\n" + "\n".join(public_examples))
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_ignored_local_files_remain_private_but_force_added_files_fail(self):
        private = self.root / "host/.env"
        private.write_text("private fixture")
        (self.root / "backups").mkdir()
        (self.root / "backups/device.bin").write_bytes(b"private fixture")
        self.assertEqual(self.run_check().returncode, 0)
        self.git("add", "-f", "host/.env", "backups/device.bin")
        result = self.run_check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("private or machine-local configuration", result.stderr)
        self.assertIn("generated artifact or private backup", result.stderr)
        self.assertNotIn("private fixture", result.stdout + result.stderr)

    def test_external_symlink_is_rejected_without_reading_its_target(self):
        target = self.root.parent / (self.root.name + "-private")
        target.write_text("private target contents")
        self.addCleanup(target.unlink)
        (self.root / "external.md").symlink_to(target)
        result = self.run_check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("symlink points outside", result.stderr)
        self.assertNotIn("private target contents", result.stdout + result.stderr)

    def test_index_symlink_is_checked_after_same_bytes_regular_replacement(self):
        path = self.root / "external.md"
        target = "../outside-fixture"
        path.symlink_to(target)
        self.git("add", "external.md")
        path.unlink()
        path.write_text(target)

        result = self.run_check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("(index): symlink points outside the repository", result.stderr)

    def test_index_shell_is_checked_after_same_bytes_symlink_replacement(self):
        path = self.root / "invalid.sh"
        contents = "if then"
        path.write_text(contents)
        self.git("add", "invalid.sh")
        path.unlink()
        path.symlink_to(contents)

        result = self.run_check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("(index): invalid shell syntax", result.stderr)

    def test_symlink_loop_fails_without_traceback_or_absolute_path(self):
        path = self.root / "loop.md"
        path.symlink_to("loop.md")
        # Path.resolve reports loops as an exception on Python 3.12 and older.
        # Newer Python versions may leave a non-strict loop unresolved instead.
        try:
            path.resolve()
        except (OSError, RuntimeError):
            pass
        else:
            self.skipTest("non-strict Path.resolve does not raise on symlink loops")

        result = self.run_check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("repository inspection failed", result.stderr)
        self.assertNotIn("Traceback", result.stdout + result.stderr)
        self.assertNotIn(str(self.root), result.stdout + result.stderr)

    def test_new_shell_scripts_are_checked_without_running_them(self):
        (self.root / "new script.sh").write_text("if then\n")
        result = self.run_check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid shell syntax", result.stderr)

    def test_additional_secret_formats_are_detected(self):
        values = ("-----BEGIN " + "PRIVATE KEY-----", "https://" + "person:secret@host.invalid",
                  "xoxb-" + "123456789012345678901234")
        for value in values:
            with self.subTest(prefix=value[:8]):
                (self.root / "candidate.md").write_text(value)
                result = self.run_check()
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn(value, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
