"""
Tests for release pipeline artifacts.

Validates file structure, syntax, and content patterns for:
- .github/workflows/release.yml
- packaging/scripts/app/preinstall, packaging/scripts/driver/preinstall, packaging/scripts/driver/postinstall
- packaging/resources/uninstall.sh, welcome.html, conclusion.html, license.html
- packaging/distribution.xml
"""

import os
import plistlib
import re
import stat
import subprocess
import unittest
import xml.etree.ElementTree as ET

# Project root is two levels up from this file
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _read(relpath):
    """Read a project file and return its contents."""
    path = os.path.join(ROOT, relpath)
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def _read_plist(relpath):
    """Read a plist file and return its parsed contents."""
    path = os.path.join(ROOT, relpath)
    with open(path, "rb") as f:
        return plistlib.load(f)


def _is_executable(relpath):
    """Check if a file has the executable bit set."""
    path = os.path.join(ROOT, relpath)
    return os.stat(path).st_mode & stat.S_IXUSR != 0


def _bash_syntax_ok(relpath):
    """Run bash -n to check shell script syntax. Returns (ok, stderr)."""
    path = os.path.join(ROOT, relpath)
    result = subprocess.run(
        ["bash", "-n", path], capture_output=True, text=True
    )
    return result.returncode == 0, result.stderr


# ---------------------------------------------------------------------------
# release.yml
# ---------------------------------------------------------------------------
class TestReleaseWorkflow(unittest.TestCase):
    """Validate .github/workflows/release.yml"""

    @classmethod
    def setUpClass(cls):
        import yaml  # PyYAML installed by runner

        cls.raw = _read(".github/workflows/release.yml")
        cls.data = yaml.safe_load(cls.raw)

    def test_yaml_parses(self):
        self.assertIsInstance(self.data, dict)

    def test_trigger_push_tags(self):
        # PyYAML turns bare `on` key into Python True
        triggers = self.data.get(True) or self.data.get("on")
        self.assertIn("push", triggers)
        self.assertIn("tags", triggers["push"])

    def test_trigger_workflow_dispatch(self):
        triggers = self.data.get(True) or self.data.get("on")
        self.assertIn("workflow_dispatch", triggers)

    def test_homebrew_no_auto_update(self):
        self.assertIn("HOMEBREW_NO_AUTO_UPDATE", self.raw)

    def test_universal_binary_archs(self):
        self.assertIn('ARCHS="arm64 x86_64"', self.raw)

    def test_only_active_arch_no(self):
        self.assertIn("ONLY_ACTIVE_ARCH=NO", self.raw)

    def test_marketing_version_override(self):
        self.assertIn("MARKETING_VERSION=", self.raw)

    def test_current_project_version_override(self):
        self.assertIn("CURRENT_PROJECT_VERSION=", self.raw)

    def test_notarization_timeout(self):
        # Notarization logic moved to packaging/scripts/notarize.sh
        notarize = _read("packaging/scripts/notarize.sh")
        self.assertIn("--timeout 15m", notarize)

    def test_notarization_error_handling(self):
        # Notarization logic moved to packaging/scripts/notarize.sh
        notarize = _read("packaging/scripts/notarize.sh")
        self.assertIn("set +e", notarize)
        self.assertIn("set -e", notarize)

    def test_curl_flags(self):
        # All curl calls should use -sfL (silent, fail, follow redirects)
        for match in re.finditer(r"curl\s+(\S+)", self.raw):
            flags = match.group(1)
            self.assertTrue(
                flags.startswith("-") and "s" in flags and "f" in flags,
                f"curl should use -sfL flags, got: curl {flags}",
            )

    def test_cleanup_removes_p12_and_cer(self):
        self.assertIn("*.p12", self.raw)
        self.assertIn("*.cer", self.raw)

    def test_sha256sums(self):
        self.assertIn("SHA256SUMS", self.raw)
        self.assertIn("shasum -a 256", self.raw)

    def test_dmg_includes_uninstall_script(self):
        self.assertIn("Uninstall MacAudio.command", self.raw)

    def test_permissions_contents_write(self):
        self.assertEqual(self.data.get("permissions", {}).get("contents"), "write")


# ---------------------------------------------------------------------------
# driver/postinstall
# ---------------------------------------------------------------------------
class TestPostinstall(unittest.TestCase):
    """Validate packaging/scripts/driver/postinstall"""

    RELPATH = "packaging/scripts/driver/postinstall"

    @classmethod
    def setUpClass(cls):
        cls.content = _read(cls.RELPATH)

    def test_shebang(self):
        self.assertTrue(self.content.startswith("#!/bin/bash"))

    def test_syntax(self):
        ok, err = _bash_syntax_ok(self.RELPATH)
        self.assertTrue(ok, f"bash -n failed: {err}")

    def test_executable(self):
        self.assertTrue(
            _is_executable(self.RELPATH),
            "postinstall must have executable permission",
        )

    def test_xattr_removal(self):
        self.assertIn("xattr -rc", self.content)

    def test_launchctl_kickstart_with_killall_fallback(self):
        self.assertIn("launchctl kickstart", self.content)
        self.assertIn("killall coreaudiod", self.content)

    def test_exit_zero(self):
        self.assertIn("exit 0", self.content)


# ---------------------------------------------------------------------------
# driver/preinstall
# ---------------------------------------------------------------------------
class TestDriverPreinstall(unittest.TestCase):
    """Validate packaging/scripts/driver/preinstall"""

    RELPATH = "packaging/scripts/driver/preinstall"

    @classmethod
    def setUpClass(cls):
        cls.content = _read(cls.RELPATH)

    def test_shebang(self):
        self.assertTrue(self.content.startswith("#!/bin/bash"))

    def test_syntax(self):
        ok, err = _bash_syntax_ok(self.RELPATH)
        self.assertTrue(ok, f"bash -n failed: {err}")

    def test_executable(self):
        self.assertTrue(
            _is_executable(self.RELPATH),
            "preinstall must have executable permission",
        )

    def test_stderr_logging(self):
        self.assertIn(">&2", self.content)

    def test_removes_existing_driver(self):
        self.assertIn("rm -rf", self.content)
        self.assertIn("MacAudioDriver.driver", self.content)

    def test_does_not_remove_app(self):
        self.assertNotIn("/Applications/MacAudio.app", self.content)

    def test_exit_zero(self):
        self.assertIn("exit 0", self.content)


# ---------------------------------------------------------------------------
# app/preinstall
# ---------------------------------------------------------------------------
class TestAppPreinstall(unittest.TestCase):
    """Validate packaging/scripts/app/preinstall"""

    RELPATH = "packaging/scripts/app/preinstall"

    @classmethod
    def setUpClass(cls):
        cls.content = _read(cls.RELPATH)

    def test_shebang(self):
        self.assertTrue(self.content.startswith("#!/bin/bash"))

    def test_syntax(self):
        ok, err = _bash_syntax_ok(self.RELPATH)
        self.assertTrue(ok, f"bash -n failed: {err}")

    def test_executable(self):
        self.assertTrue(
            _is_executable(self.RELPATH),
            "preinstall must have executable permission",
        )

    def test_stderr_logging(self):
        self.assertIn(">&2", self.content)

    def test_stops_running_app(self):
        self.assertIn("killall", self.content)
        self.assertIn("MacAudio", self.content)

    def test_removes_existing_app(self):
        self.assertIn("rm -rf", self.content)
        self.assertIn("/Applications/MacAudio.app", self.content)

    def test_exit_zero(self):
        self.assertIn("exit 0", self.content)


# ---------------------------------------------------------------------------
# uninstall.sh
# ---------------------------------------------------------------------------
class TestUninstallScript(unittest.TestCase):
    """Validate packaging/resources/uninstall.sh"""

    RELPATH = "packaging/resources/uninstall.sh"

    @classmethod
    def setUpClass(cls):
        cls.content = _read(cls.RELPATH)

    def test_shebang(self):
        self.assertTrue(self.content.startswith("#!/bin/bash"))

    def test_syntax(self):
        ok, err = _bash_syntax_ok(self.RELPATH)
        self.assertTrue(ok, f"bash -n failed: {err}")

    def test_executable(self):
        self.assertTrue(
            _is_executable(self.RELPATH),
            "uninstall.sh must have executable permission",
        )

    def test_confirmation_prompt(self):
        self.assertRegex(self.content, r'read\s+-p')

    def test_restarts_coreaudiod(self):
        self.assertIn("launchctl kickstart", self.content)
        self.assertIn("killall coreaudiod", self.content)

    def test_removes_app(self):
        self.assertIn("/Applications/MacAudio.app", self.content)
        self.assertIn("rm -rf", self.content)

    def test_removes_driver(self):
        self.assertIn(
            "/Library/Audio/Plug-Ins/HAL/MacAudioDriver.driver", self.content
        )


# ---------------------------------------------------------------------------
# distribution.xml
# ---------------------------------------------------------------------------
class TestDistributionXML(unittest.TestCase):
    """Validate packaging/distribution.xml"""

    RELPATH = "packaging/distribution.xml"

    @classmethod
    def setUpClass(cls):
        cls.content = _read(cls.RELPATH)
        cls.tree = ET.parse(os.path.join(ROOT, cls.RELPATH))
        cls.root = cls.tree.getroot()

    def test_xml_valid(self):
        # If we got here, ET.parse succeeded
        self.assertIsNotNone(self.root)

    def test_xmllint(self):
        path = os.path.join(ROOT, self.RELPATH)
        result = subprocess.run(
            ["xmllint", "--noout", path], capture_output=True, text=True
        )
        self.assertEqual(result.returncode, 0, f"xmllint failed: {result.stderr}")

    def test_install_check_js_function(self):
        self.assertIn("function installCheck()", self.content)

    def test_min_version_14_2(self):
        os_version = self.root.find(".//os-version")
        self.assertIsNotNone(os_version, "os-version element not found")
        self.assertEqual(os_version.get("min"), "14.2")

    def test_welcome_reference(self):
        welcome = self.root.find('.//welcome[@file="welcome.html"]')
        self.assertIsNotNone(welcome, "welcome.html reference not found")

    def test_license_reference(self):
        lic = self.root.find('.//license[@file="license.html"]')
        self.assertIsNotNone(lic, "license.html reference not found")

    def test_conclusion_reference(self):
        conclusion = self.root.find('.//conclusion[@file="conclusion.html"]')
        self.assertIsNotNone(conclusion, "conclusion.html reference not found")

    def test_both_pkg_refs(self):
        pkg_refs = [el.get("id") for el in self.root.findall(".//pkg-ref")]
        self.assertIn("com.macaudio.app.pkg", pkg_refs)
        self.assertIn("com.macaudio.driver.pkg", pkg_refs)


# ---------------------------------------------------------------------------
# welcome.html
# ---------------------------------------------------------------------------
class TestWelcomeHTML(unittest.TestCase):
    """Validate packaging/resources/welcome.html"""

    @classmethod
    def setUpClass(cls):
        cls.content = _read("packaging/resources/welcome.html")

    def test_lang_en(self):
        self.assertIn('lang="en"', self.content)

    def test_dark_mode_css(self):
        self.assertIn("prefers-color-scheme: dark", self.content)

    def test_permissions_warning(self):
        self.assertIn("Permissions required", self.content)

    def test_note_border(self):
        self.assertRegex(self.content, r'\.note\s*\{[^}]*border')


# ---------------------------------------------------------------------------
# conclusion.html
# ---------------------------------------------------------------------------
class TestConclusionHTML(unittest.TestCase):
    """Validate packaging/resources/conclusion.html"""

    @classmethod
    def setUpClass(cls):
        cls.content = _read("packaging/resources/conclusion.html")

    def test_lang_en(self):
        self.assertIn('lang="en"', self.content)

    def test_dark_mode_css(self):
        self.assertIn("prefers-color-scheme: dark", self.content)

    def test_troubleshooting_section(self):
        self.assertIn("Troubleshooting", self.content)

    def test_uninstalling_section(self):
        self.assertIn("Uninstalling", self.content)

    def test_github_issues_link(self):
        self.assertRegex(
            self.content,
            r'https://github\.com/cortexuvula/MacAudio/issues',
        )


# ---------------------------------------------------------------------------
# license.html
# ---------------------------------------------------------------------------
class TestLicenseHTML(unittest.TestCase):
    """Validate packaging/resources/license.html"""

    @classmethod
    def setUpClass(cls):
        cls.content = _read("packaging/resources/license.html")

    def test_mit_text(self):
        self.assertIn("MIT License", self.content)
        self.assertIn("Permission is hereby granted", self.content)

    def test_dark_mode_css(self):
        self.assertIn("prefers-color-scheme: dark", self.content)


# ---------------------------------------------------------------------------
# notarize.sh
# ---------------------------------------------------------------------------
class TestNotarizeScript(unittest.TestCase):
    """Validate packaging/scripts/notarize.sh"""

    RELPATH = "packaging/scripts/notarize.sh"

    @classmethod
    def setUpClass(cls):
        cls.content = _read(cls.RELPATH)

    def test_shebang(self):
        self.assertTrue(self.content.startswith("#!/bin/bash"))

    def test_syntax(self):
        ok, err = _bash_syntax_ok(self.RELPATH)
        self.assertTrue(ok, f"bash -n failed: {err}")

    def test_executable(self):
        self.assertTrue(
            _is_executable(self.RELPATH),
            "notarize.sh must have executable permission",
        )

    def test_strict_mode(self):
        self.assertIn("set -euo pipefail", self.content)

    def test_apple_id_auth(self):
        """Must use Apple ID auth for notarization."""
        self.assertIn("--apple-id", self.content)
        self.assertIn("--password", self.content)
        self.assertIn("--team-id", self.content)

    def test_timeout(self):
        self.assertIn("--timeout 15m", self.content)

    def test_error_handling(self):
        self.assertIn("set +e", self.content,
                       "notarize.sh must disable errexit around notarytool submit")
        self.assertIn("set -e", self.content,
                       "notarize.sh must re-enable errexit after notarytool submit")

    def test_staple(self):
        self.assertIn("stapler staple", self.content,
                       "notarize.sh must staple the notarization ticket")

    def test_status_check(self):
        self.assertIn("Accepted", self.content,
                       "notarize.sh must check for 'Accepted' status")


# ---------------------------------------------------------------------------
# generate.sh
# ---------------------------------------------------------------------------
class TestGenerateScript(unittest.TestCase):
    """Validate generate.sh (xcodegen wrapper with recovery)."""

    RELPATH = "generate.sh"

    @classmethod
    def setUpClass(cls):
        cls.content = _read(cls.RELPATH)

    def test_shebang(self):
        self.assertTrue(self.content.startswith("#!/bin/bash"))

    def test_syntax(self):
        ok, err = _bash_syntax_ok(self.RELPATH)
        self.assertTrue(ok, f"bash -n failed: {err}")

    def test_runs_xcodegen(self):
        self.assertIn("xcodegen generate", self.content,
                       "generate.sh must run xcodegen generate")

    def test_restores_entitlements(self):
        self.assertIn("audio-input", self.content,
                       "generate.sh must restore com.apple.security.device.audio-input entitlement")
        self.assertIn("entitlements", self.content.lower())

    def test_restores_cf_bundle_executable(self):
        self.assertIn("CFBundleExecutable", self.content,
                       "generate.sh must restore CFBundleExecutable in driver Info.plist")

    def test_strict_mode(self):
        self.assertIn("set -euo pipefail", self.content)


# ---------------------------------------------------------------------------
# Installer/install.sh
# ---------------------------------------------------------------------------
class TestDevInstallScript(unittest.TestCase):
    """Validate Installer/install.sh (dev install script)."""

    RELPATH = "Installer/install.sh"

    @classmethod
    def setUpClass(cls):
        cls.content = _read(cls.RELPATH)

    def test_shebang(self):
        self.assertTrue(self.content.startswith("#!/bin/bash"))

    def test_syntax(self):
        ok, err = _bash_syntax_ok(self.RELPATH)
        self.assertTrue(ok, f"bash -n failed: {err}")

    def test_strict_mode(self):
        self.assertIn("set -euo pipefail", self.content,
                       "install.sh must use set -euo pipefail")

    def test_xattr_removal(self):
        self.assertIn("xattr -rc", self.content,
                       "install.sh must clear xattrs after copy")

    def test_launchctl_kickstart(self):
        self.assertIn("launchctl kickstart", self.content,
                       "install.sh must use launchctl kickstart to restart coreaudiod")


# ---------------------------------------------------------------------------
# Installer/uninstall.sh
# ---------------------------------------------------------------------------
class TestDevUninstallScript(unittest.TestCase):
    """Validate Installer/uninstall.sh (dev uninstall script)."""

    RELPATH = "Installer/uninstall.sh"

    @classmethod
    def setUpClass(cls):
        cls.content = _read(cls.RELPATH)

    def test_shebang(self):
        self.assertTrue(self.content.startswith("#!/bin/bash"))

    def test_syntax(self):
        ok, err = _bash_syntax_ok(self.RELPATH)
        self.assertTrue(ok, f"bash -n failed: {err}")

    def test_strict_mode(self):
        self.assertIn("set -euo pipefail", self.content,
                       "uninstall.sh must use set -euo pipefail")


# ---------------------------------------------------------------------------
# Component plists — disable bundle relocation
# ---------------------------------------------------------------------------
class TestAppComponentPlist(unittest.TestCase):
    """Validate packaging/app-component.plist disables relocation."""

    @classmethod
    def setUpClass(cls):
        cls.plist = _read_plist("packaging/app-component.plist")

    def test_is_array(self):
        self.assertIsInstance(self.plist, list,
                              "Component plist must be an array")

    def test_has_entries(self):
        self.assertGreaterEqual(len(self.plist), 1,
                                "Component plist must have at least one entry")

    def test_app_not_relocatable(self):
        self.assertFalse(self.plist[0].get("BundleIsRelocatable", True),
                         "App bundle must have BundleIsRelocatable=false")

    def test_embedded_driver_not_relocatable(self):
        self.assertGreaterEqual(len(self.plist), 2,
                                "Must have entry for embedded MacAudioDriver.driver")
        self.assertFalse(self.plist[1].get("BundleIsRelocatable", True),
                         "Embedded driver must have BundleIsRelocatable=false")

    def test_overwrite_action_upgrade(self):
        self.assertEqual(self.plist[0].get("BundleOverwriteAction"), "upgrade")


class TestDriverComponentPlist(unittest.TestCase):
    """Validate packaging/driver-component.plist disables relocation."""

    @classmethod
    def setUpClass(cls):
        cls.plist = _read_plist("packaging/driver-component.plist")

    def test_is_array(self):
        self.assertIsInstance(self.plist, list)

    def test_has_one_entry(self):
        self.assertEqual(len(self.plist), 1)

    def test_driver_not_relocatable(self):
        self.assertFalse(self.plist[0].get("BundleIsRelocatable", True),
                         "Driver must have BundleIsRelocatable=false")

    def test_overwrite_action_upgrade(self):
        self.assertEqual(self.plist[0].get("BundleOverwriteAction"), "upgrade")


# ---------------------------------------------------------------------------
# .gitignore — __pycache__ coverage
# ---------------------------------------------------------------------------
class TestGitignore(unittest.TestCase):
    """Validate .gitignore has required entries."""

    @classmethod
    def setUpClass(cls):
        cls.content = _read(".gitignore")

    def test_pycache(self):
        self.assertIn("__pycache__", self.content,
                       ".gitignore must exclude __pycache__/")

    def test_pyc(self):
        self.assertIn("*.pyc", self.content,
                       ".gitignore must exclude *.pyc files")


# ---------------------------------------------------------------------------
# test-pipeline.yml — project.yml trigger
# ---------------------------------------------------------------------------
class TestTestPipeline(unittest.TestCase):
    """Validate .github/workflows/test-pipeline.yml"""

    @classmethod
    def setUpClass(cls):
        cls.content = _read(".github/workflows/test-pipeline.yml")

    def test_triggers_on_project_yml(self):
        self.assertIn("project.yml", self.content,
                       "test-pipeline.yml must trigger on project.yml changes")


if __name__ == "__main__":
    unittest.main()
