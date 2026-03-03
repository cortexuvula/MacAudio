"""
Source integrity tests for MacAudio app and driver code.

Validates that constants match across Swift/C boundaries, critical code patterns
exist, and xcodegen-vulnerable files are intact. Runs as pure text analysis —
no Xcode or compilation required.
"""

import os
import re
import unittest
import plistlib

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def _read(relpath):
    with open(os.path.join(PROJECT_ROOT, relpath), "r") as f:
        return f.read()

def _read_plist(relpath):
    with open(os.path.join(PROJECT_ROOT, relpath), "rb") as f:
        return plistlib.load(f)


# ---------------------------------------------------------------------------
# Constant Sync — Swift ↔ C must agree
# ---------------------------------------------------------------------------

class TestConstantSync(unittest.TestCase):
    """Constants must match between AudioConstants.swift, SharedRingBuffer.h, and MacAudioDriver.h."""

    @classmethod
    def setUpClass(cls):
        cls.swift = _read("MacAudio/AudioEngine/AudioConstants.swift")
        cls.rb_h = _read("MacAudioDriver/SharedRingBuffer.h")
        cls.drv_h = _read("MacAudioDriver/MacAudioDriver.h")

    def test_ring_buffer_frames(self):
        swift_val = re.search(r"ringBufferFrames\b.*?=\s*(\d+)", self.swift)
        rb_val = re.search(r"kRingBufferFrames\s+(\d+)", self.rb_h)
        drv_val = re.search(r"kDevice_RingBufferFrames\s+(\d+)", self.drv_h)
        self.assertIsNotNone(swift_val, "ringBufferFrames not in Swift")
        self.assertIsNotNone(rb_val, "kRingBufferFrames not in header")
        self.assertIsNotNone(drv_val, "kDevice_RingBufferFrames not in driver header")
        self.assertEqual(swift_val.group(1), rb_val.group(1),
                         "ringBufferFrames mismatch: Swift vs SharedRingBuffer.h")
        self.assertEqual(swift_val.group(1), drv_val.group(1),
                         "ringBufferFrames mismatch: Swift vs MacAudioDriver.h")

    def test_num_channels(self):
        swift_val = re.search(r"numChannels\b.*?=\s*(\d+)", self.swift)
        rb_val = re.search(r"kNumChannels\s+(\d+)", self.rb_h)
        drv_val = re.search(r"kDevice_NumChannels\s+(\d+)", self.drv_h)
        self.assertIsNotNone(swift_val)
        self.assertIsNotNone(rb_val)
        self.assertIsNotNone(drv_val)
        self.assertEqual(swift_val.group(1), rb_val.group(1))
        self.assertEqual(swift_val.group(1), drv_val.group(1))

    def test_default_sample_rate(self):
        swift_val = re.search(r"defaultSampleRate\b.*?=\s*([\d.]+)", self.swift)
        drv_val = re.search(r"kDevice_DefaultSampleRate\s+([\d.]+)", self.drv_h)
        rb_init = re.search(r"sampleRate,\s*(\d+)", _read("MacAudioDriver/SharedRingBuffer.c"))
        self.assertIsNotNone(swift_val)
        self.assertIsNotNone(drv_val)
        self.assertIsNotNone(rb_init)
        self.assertEqual(float(swift_val.group(1)), float(drv_val.group(1)))
        self.assertEqual(int(float(swift_val.group(1))), int(rb_init.group(1)))

    def test_shm_name(self):
        swift_val = re.search(r'shmName\s*=\s*"([^"]+)"', self.swift)
        rb_val = re.search(r'kSHM_Name\s+"([^"]+)"', self.rb_h)
        self.assertIsNotNone(swift_val, "shmName not found in Swift")
        self.assertIsNotNone(rb_val, "kSHM_Name not found in header")
        self.assertEqual(swift_val.group(1), rb_val.group(1))

    def test_device_uid(self):
        swift_val = re.search(r'virtualDeviceUID\s*=\s*"([^"]+)"', self.swift)
        drv_val = re.search(r'kDevice_UID\s+"([^"]+)"', self.drv_h)
        self.assertIsNotNone(swift_val)
        self.assertIsNotNone(drv_val)
        self.assertEqual(swift_val.group(1), drv_val.group(1))

    def test_supported_sample_rates(self):
        # Swift: supportedSampleRates: [Float64] = [44100.0, 48000.0, 96000.0]
        # Match the second bracket group (the values, not the type)
        swift_match = re.search(r"supportedSampleRates\b.*?=\s*\[([^\]]+)\]", self.swift)
        self.assertIsNotNone(swift_match, "supportedSampleRates not found in Swift")
        swift_rates = re.findall(r"([\d.]+)", swift_match.group(1))
        # Definition moved from header to .c file (extern in header)
        drv_c = _read("MacAudioDriver/MacAudioDriver.c")
        drv_match = re.search(
            r"kSupportedSampleRates\[.*?\]\s*=\s*\{([^}]+)\}", drv_c)
        self.assertIsNotNone(drv_match, "kSupportedSampleRates definition not found in driver .c")
        drv_rates = re.findall(r"([\d.]+)", drv_match.group(1))
        self.assertEqual(
            [float(r) for r in swift_rates],
            [float(r) for r in drv_rates],
            "Supported sample rates mismatch between Swift and driver")


# ---------------------------------------------------------------------------
# Driver Structure — HAL plugin invariants
# ---------------------------------------------------------------------------

class TestDriverStructure(unittest.TestCase):
    """Structural invariants of the AudioServerPlugin driver."""

    @classmethod
    def setUpClass(cls):
        cls.drv_c = _read("MacAudioDriver/MacAudioDriver.c")
        cls.drv_h = _read("MacAudioDriver/MacAudioDriver.h")
        cls.info_plist = _read_plist("MacAudioDriver/Info.plist")

    def test_vtable_has_23_entries(self):
        """AudioServerPlugInDriverInterface must have exactly 23 function pointers (incl. NULL)."""
        match = re.search(
            r"gDriverInterface\s*=\s*\{([^}]+)\}",
            self.drv_c, re.DOTALL)
        self.assertIsNotNone(match)
        entries = [e.strip() for e in match.group(1).split(",") if e.strip()]
        self.assertEqual(len(entries), 23,
                         f"Expected 23 vtable entries, got {len(entries)}: {entries}")

    def test_vtable_starts_with_null(self):
        match = re.search(
            r"gDriverInterface\s*=\s*\{\s*(\w+)",
            self.drv_c, re.DOTALL)
        self.assertIsNotNone(match)
        self.assertEqual(match.group(1), "NULL")

    def test_query_interface_standard_uuid(self):
        self.assertIn("EAAF5B97-B965-4F68-9815-E718330862D5", self.drv_c,
                       "Standard AudioServerPlugIn UUID missing from QueryInterface")

    def test_query_interface_tahoe_uuid(self):
        self.assertIn("EEA5773D-CC43-49F1-8E00-8F96E7D23B17", self.drv_c,
                       "Tahoe AudioServerPlugIn UUID missing from QueryInterface")

    def test_query_interface_iunknown_uuid(self):
        # IUnknown UUID is constructed from bytes: 0x00...0xC0,0x00...0x46
        # Check for the characteristic byte pattern in CFUUIDGetConstantUUIDWithBytes
        self.assertRegex(self.drv_c,
                         r"0xC0,\s*0x00,\s*0x00,\s*0x00,\s*0x00,\s*0x00,\s*0x00,\s*0x46",
                         "IUnknown UUID bytes missing from QueryInterface")

    def test_plugin_type_uuid_matches_info_plist(self):
        plist_types = self.info_plist.get("CFPlugInTypes", {})
        self.assertTrue(len(plist_types) > 0, "No CFPlugInTypes in Info.plist")
        type_uuid = list(plist_types.keys())[0]
        # The type UUID is used in MacAudioDriver_Create to validate the requested type
        self.assertTrue(
            type_uuid in self.drv_h or type_uuid in self.drv_c,
            f"Plugin type UUID {type_uuid} from Info.plist not found in driver source")

    def test_factory_uuid_in_info_plist(self):
        factories = self.info_plist.get("CFPlugInFactories", {})
        self.assertTrue(len(factories) > 0, "No CFPlugInFactories in Info.plist")
        factory_uuid = list(factories.keys())[0]
        # The factory function name must appear in driver source
        factory_func = factories[factory_uuid]
        self.assertTrue(
            factory_func in self.drv_h or factory_func in self.drv_c,
            f"Factory function {factory_func} for UUID {factory_uuid} not found in driver source")

    def test_object_ids_sequential(self):
        ids = re.findall(r"kObjectID_\w+\s*=\s*(\w+)", self.drv_h)
        # First is kAudioObjectPlugInObject (1), rest are 2,3,4,5
        numeric = []
        for v in ids:
            if v == "kAudioObjectPlugInObject":
                numeric.append(1)
            else:
                numeric.append(int(v))
        self.assertEqual(numeric, list(range(1, len(numeric) + 1)),
                         f"Object IDs not sequential: {numeric}")

    def test_doio_reads_ring_buffer(self):
        self.assertIn("SharedRingBuffer_Read", self.drv_c,
                       "DoIOOperation must read from the ring buffer")

    def test_startio_opens_ring_buffer(self):
        self.assertIn("SharedRingBuffer_CreateOrOpen", self.drv_c,
                       "StartIO must open the ring buffer")

    def test_stopio_closes_ring_buffer(self):
        self.assertIn("SharedRingBuffer_Close", self.drv_c,
                       "StopIO must close the ring buffer")


# ---------------------------------------------------------------------------
# SharedRingBuffer API — C code invariants
# ---------------------------------------------------------------------------

class TestSharedRingBufferAPI(unittest.TestCase):
    """Validates ring buffer C implementation invariants via source inspection."""

    @classmethod
    def setUpClass(cls):
        cls.rb_h = _read("MacAudioDriver/SharedRingBuffer.h")
        cls.rb_c = _read("MacAudioDriver/SharedRingBuffer.c")

    def test_buffer_size_power_of_two(self):
        match = re.search(r"kRingBufferFrames\s+(\d+)", self.rb_h)
        self.assertIsNotNone(match)
        val = int(match.group(1))
        self.assertTrue(val > 0 and (val & (val - 1)) == 0,
                         f"kRingBufferFrames={val} is not a power of 2")

    def test_bitmask_wraparound(self):
        self.assertIn("kRingBufferFrames - 1", self.rb_c,
                       "Ring buffer must use bitmask (kRingBufferFrames - 1) for wraparound")

    def test_atomic_heads(self):
        self.assertIn("_Atomic uint64_t writeHeadFrames", self.rb_c)
        self.assertIn("_Atomic uint64_t readHeadFrames", self.rb_c)

    def test_acquire_release_ordering(self):
        self.assertIn("memory_order_acquire", self.rb_c)
        self.assertIn("memory_order_release", self.rb_c)

    def test_null_guards(self):
        """All public functions must guard against NULL rb."""
        for func in ["Write", "Read", "GetActive", "GetSampleRate",
                      "GetWriteHead", "GetReadHead", "SetActive", "SetSampleRate"]:
            fn_name = f"SharedRingBuffer_{func}"
            # Find the function body and check for a NULL guard
            pattern = rf"{fn_name}\([^)]*\)\s*\{{[^}}]*(!rb|rb\))"
            self.assertRegex(self.rb_c, pattern,
                             f"{fn_name} missing NULL guard")

    def test_opaque_type_in_header(self):
        self.assertIn("typedef struct SharedRingBuffer SharedRingBuffer", self.rb_h,
                       "SharedRingBuffer must be an opaque type in the header")
        self.assertNotIn("float buffer", self.rb_h,
                          "Header must not expose internal buffer field")


# ---------------------------------------------------------------------------
# Entitlements — catches xcodegen wipe
# ---------------------------------------------------------------------------

class TestEntitlements(unittest.TestCase):
    """MacAudio.entitlements must contain required entitlements."""

    @classmethod
    def setUpClass(cls):
        cls.plist = _read_plist("MacAudio/MacAudio.entitlements")

    def test_audio_input_entitlement(self):
        self.assertTrue(
            self.plist.get("com.apple.security.device.audio-input", False),
            "Missing com.apple.security.device.audio-input entitlement")

    def test_not_empty_dict(self):
        self.assertTrue(len(self.plist) > 0,
                         "Entitlements file is an empty dict (xcodegen damage)")


# ---------------------------------------------------------------------------
# Driver Info.plist — catches xcodegen strip
# ---------------------------------------------------------------------------

class TestDriverInfoPlist(unittest.TestCase):
    """MacAudioDriver/Info.plist must have keys that xcodegen strips."""

    @classmethod
    def setUpClass(cls):
        cls.plist = _read_plist("MacAudioDriver/Info.plist")

    def test_has_cf_bundle_executable(self):
        self.assertIn("CFBundleExecutable", self.plist,
                       "CFBundleExecutable missing from driver Info.plist (xcodegen damage)")

    def test_has_cf_plugin_factories(self):
        factories = self.plist.get("CFPlugInFactories", {})
        self.assertTrue(len(factories) > 0,
                         "CFPlugInFactories missing or empty")

    def test_has_cf_plugin_types(self):
        types = self.plist.get("CFPlugInTypes", {})
        self.assertTrue(len(types) > 0,
                         "CFPlugInTypes missing or empty")


# ---------------------------------------------------------------------------
# Swift Source Integrity — critical code patterns
# ---------------------------------------------------------------------------

class TestSwiftSourceIntegrity(unittest.TestCase):
    """Critical patterns that must exist in Swift source files."""

    def test_audio_mixer_hard_clips(self):
        src = _read("MacAudio/AudioEngine/AudioMixer.swift")
        self.assertRegex(src, r"max\s*\(\s*-1\.0.*min\s*\(\s*1\.0",
                         "AudioMixer must hard-clip output to [-1.0, 1.0]")

    def test_audio_mixer_handles_mono_to_stereo(self):
        src = _read("MacAudio/AudioEngine/AudioMixer.swift")
        # Should handle mono input by duplicating to stereo
        self.assertRegex(src, r"[Mm]ono.*stereo|1\s*==.*channels?|channels?\s*==\s*1",
                         "AudioMixer must handle mono→stereo conversion")

    def test_device_manager_skips_virtual_device(self):
        src = _read("MacAudio/Models/AudioDeviceManager.swift")
        self.assertIn("virtualDeviceUID", src,
                       "AudioDeviceManager must filter out the virtual device")

    def test_ring_buffer_writer_calls_set_active(self):
        src = _read("MacAudio/AudioEngine/SharedRingBufferWriter.swift")
        self.assertIn("SetActive", src,
                       "SharedRingBufferWriter must call SetActive on open/close")

    def test_app_state_guards_driver_installed(self):
        src = _read("MacAudio/Models/AppState.swift")
        self.assertIn("driverInstalled", src,
                       "AppState must guard on driverInstalled before starting audio")


# ---------------------------------------------------------------------------
# Tier 1 — Atomic safety in driver
# ---------------------------------------------------------------------------

class TestDriverAtomicSafety(unittest.TestCase):
    """Validates Tier 1 atomic safety changes in the C driver."""

    @classmethod
    def setUpClass(cls):
        cls.drv_c = _read("MacAudioDriver/MacAudioDriver.c")

    def test_stdatomic_included(self):
        self.assertIn("#include <stdatomic.h>", self.drv_c,
                       "Driver must include stdatomic.h for atomic types")

    def test_gRingBuffer_is_atomic(self):
        self.assertRegex(self.drv_c, r"_Atomic\(SharedRingBuffer\*\)\s+gRingBuffer",
                         "gRingBuffer must be _Atomic(SharedRingBuffer*)")

    def test_gVolume_is_atomic(self):
        self.assertRegex(self.drv_c, r"_Atomic\s+Float32\s+gVolume_Input_Master",
                         "gVolume_Input_Master must be _Atomic Float32")

    def test_gMute_is_atomic(self):
        self.assertRegex(self.drv_c, r"_Atomic\s+bool\s+gMute_Input_Master",
                         "gMute_Input_Master must be _Atomic bool")

    def test_gDevice_HostTicksPerFrame_is_atomic(self):
        self.assertRegex(self.drv_c, r"_Atomic\s+double\s+gDevice_HostTicksPerFrame",
                         "gDevice_HostTicksPerFrame must be _Atomic double")

    def test_gDevice_AnchorHostTime_is_atomic(self):
        self.assertRegex(self.drv_c, r"_Atomic\s+uint64_t\s+gDevice_AnchorHostTime",
                         "gDevice_AnchorHostTime must be _Atomic uint64_t")

    def test_doio_uses_atomic_load_for_ring_buffer(self):
        """DoIOOperation must use atomic_load_explicit to read gRingBuffer."""
        self.assertRegex(self.drv_c, r"atomic_load_explicit\(&gRingBuffer",
                         "DoIOOperation must atomic_load gRingBuffer")

    def test_doio_uses_atomic_load_for_mute(self):
        self.assertRegex(self.drv_c, r"atomic_load_explicit\(&gMute_Input_Master",
                         "DoIOOperation must atomic_load gMute_Input_Master")

    def test_doio_uses_atomic_load_for_volume(self):
        self.assertRegex(self.drv_c, r"atomic_load_explicit\(&gVolume_Input_Master",
                         "DoIOOperation must atomic_load gVolume_Input_Master")

    def test_get_zero_timestamp_uses_atomic_loads(self):
        self.assertRegex(self.drv_c, r"atomic_load_explicit\(&gDevice_AnchorHostTime",
                         "GetZeroTimeStamp must atomic_load gDevice_AnchorHostTime")
        self.assertRegex(self.drv_c, r"atomic_load_explicit\(&gDevice_HostTicksPerFrame",
                         "GetZeroTimeStamp must atomic_load gDevice_HostTicksPerFrame")

    def test_recalc_helper_exists(self):
        self.assertIn("recalcHostTicksPerFrame", self.drv_c,
                       "recalcHostTicksPerFrame helper must exist")


# ---------------------------------------------------------------------------
# Tier 1 — inDataSize validation (buffer overflow protection)
# ---------------------------------------------------------------------------

class TestDriverDataSizeValidation(unittest.TestCase):
    """Validates inDataSize checks in GetPropertyData/SetPropertyData."""

    @classmethod
    def setUpClass(cls):
        cls.drv_c = _read("MacAudioDriver/MacAudioDriver.c")

    def test_get_property_data_validates_inDataSize(self):
        """GetPropertyData must check inDataSize before writing to outData."""
        count = self.drv_c.count("if (inDataSize < sizeof(")
        self.assertGreaterEqual(count, 10,
                                f"GetPropertyData should validate inDataSize in many cases, found {count}")

    def test_get_property_data_returns_bad_size_error(self):
        self.assertIn("kAudioHardwareBadPropertySizeError", self.drv_c,
                       "Driver must return kAudioHardwareBadPropertySizeError for undersized buffers")

    def test_qualifier_data_null_check(self):
        """inQualifierData must be checked for NULL before dereferencing."""
        self.assertRegex(self.drv_c, r"!inQualifierData",
                         "Driver must check inQualifierData for NULL")


# ---------------------------------------------------------------------------
# Tier 2 — HAL property change notifications
# ---------------------------------------------------------------------------

class TestDriverNotifications(unittest.TestCase):
    """Validates the driver notifies clients of property changes."""

    @classmethod
    def setUpClass(cls):
        cls.drv_c = _read("MacAudioDriver/MacAudioDriver.c")

    def test_properties_changed_called(self):
        """SetPropertyData must call PropertiesChanged for sample rate and stream format."""
        count = self.drv_c.count("PropertiesChanged")
        self.assertGreaterEqual(count, 2,
                                f"Expected >= 2 PropertiesChanged calls, found {count}")

    def test_request_device_configuration_change(self):
        self.assertIn("RequestDeviceConfigurationChange", self.drv_c,
                       "Driver must call RequestDeviceConfigurationChange for sample rate updates")


# ---------------------------------------------------------------------------
# Tier 1/2 — Swift thread safety & correctness
# ---------------------------------------------------------------------------

class TestSwiftThreadSafety(unittest.TestCase):
    """Validates Swift thread safety patterns from Tier 1 and 2."""

    def test_audio_mixer_uses_os_unfair_lock_for_gain(self):
        src = _read("MacAudio/AudioEngine/AudioMixer.swift")
        self.assertIn("os_unfair_lock", src,
                       "AudioMixer must use os_unfair_lock for gain access")
        # Must NOT use NSLock (Tier 1.2)
        self.assertNotIn("NSLock", src,
                          "AudioMixer must NOT use NSLock on real-time audio thread")

    def test_audio_mixer_uses_os_unfair_lock_for_mic_buffer(self):
        src = _read("MacAudio/AudioEngine/AudioMixer.swift")
        self.assertIn("micBufferLock", src,
                       "AudioMixer must have micBufferLock for mic buffer sync")

    def test_app_state_is_main_actor(self):
        src = _read("MacAudio/Models/AppState.swift")
        self.assertIn("@MainActor", src,
                       "AppState must be annotated with @MainActor")

    def test_app_state_deinit_removes_listener(self):
        src = _read("MacAudio/Models/AppState.swift")
        self.assertIn("AudioObjectRemovePropertyListenerBlock", src,
                       "AppState deinit must remove audio property listener block")

    def test_app_state_calls_destroy_shared_memory(self):
        src = _read("MacAudio/Models/AppState.swift")
        self.assertIn("destroySharedMemory", src,
                       "AppState.stopAudio must call destroySharedMemory to clean up shm")

    def test_audio_mixer_has_destroy_shared_memory(self):
        src = _read("MacAudio/AudioEngine/AudioMixer.swift")
        self.assertIn("destroySharedMemory", src,
                       "AudioMixer must expose destroySharedMemory method")

    def test_ring_buffer_writer_has_destroy(self):
        src = _read("MacAudio/AudioEngine/SharedRingBufferWriter.swift")
        self.assertIn("SharedRingBuffer_Destroy", src,
                       "SharedRingBufferWriter.destroy() must call SharedRingBuffer_Destroy")


# ---------------------------------------------------------------------------
# Tier 2 — NSLog elimination
# ---------------------------------------------------------------------------

class TestNoNSLogInProduction(unittest.TestCase):
    """No NSLog calls should remain — all logging uses os.Logger."""

    def test_app_state_no_nslog(self):
        src = _read("MacAudio/Models/AppState.swift")
        self.assertNotIn("NSLog(", src,
                          "AppState must not use NSLog — use os.Logger instead")

    def test_app_delegate_no_nslog(self):
        src = _read("MacAudio/MacAudioApp.swift")
        self.assertNotIn("NSLog(", src,
                          "AppDelegate must not use NSLog — use os.Logger instead")

    def test_app_state_uses_os_logger(self):
        src = _read("MacAudio/Models/AppState.swift")
        self.assertIn("Logger(subsystem:", src,
                       "AppState must use os.Logger")

    def test_app_delegate_uses_os_logger(self):
        src = _read("MacAudio/MacAudioApp.swift")
        self.assertIn("Logger(subsystem:", src,
                       "AppDelegate must use os.Logger")


# ---------------------------------------------------------------------------
# Tier 2 — UserDefaults persistence
# ---------------------------------------------------------------------------

class TestUserDefaultsPersistence(unittest.TestCase):
    """AppState must persist user preferences via UserDefaults."""

    @classmethod
    def setUpClass(cls):
        cls.src = _read("MacAudio/Models/AppState.swift")

    def test_save_preferences_exists(self):
        self.assertIn("savePreferences", self.src,
                       "AppState must have savePreferences method")

    def test_load_preferences_exists(self):
        self.assertIn("loadPreferences", self.src,
                       "AppState must have loadPreferences method")

    def test_user_defaults_used(self):
        self.assertIn("UserDefaults.standard", self.src,
                       "AppState must use UserDefaults.standard for persistence")

    def test_mic_volume_key(self):
        self.assertIn("micVolume", self.src)

    def test_system_volume_key(self):
        self.assertIn("systemVolume", self.src)


# ---------------------------------------------------------------------------
# Tier 2 — Dead code was removed
# ---------------------------------------------------------------------------

class TestDeadCodeRemoved(unittest.TestCase):
    """Dead code must not exist in the codebase."""

    def test_no_views_directory(self):
        views_dir = os.path.join(PROJECT_ROOT, "MacAudio", "Views")
        self.assertFalse(os.path.isdir(views_dir),
                          "MacAudio/Views/ directory should be deleted (dead SwiftUI views)")

    def test_no_permissions_utility(self):
        path = os.path.join(PROJECT_ROOT, "MacAudio", "Utilities", "Permissions.swift")
        self.assertFalse(os.path.exists(path),
                          "Permissions.swift should be deleted (dead code)")

    def test_no_uninstall_driver_function(self):
        src = _read("MacAudio/Utilities/DriverInstaller.swift")
        self.assertNotIn("func uninstallDriver", src,
                          "uninstallDriver() should be deleted (dead code)")

    def test_no_isReady_property(self):
        src = _read("MacAudio/Models/AppState.swift")
        self.assertNotIn("isReady", src,
                          "isReady property should be deleted (dead code)")

    def test_no_double_dispatch_in_app_delegate(self):
        src = _read("MacAudio/MacAudioApp.swift")
        # The objectWillChange sink should use receive(on:) but NOT also DispatchQueue.main.async inside
        # Count occurrences of DispatchQueue.main.async — should be zero in the sink block
        self.assertNotIn("DispatchQueue.main.async", src,
                          "AppDelegate should not have double dispatch (receive(on:) + DispatchQueue.main.async)")

    def test_no_double_dispatch_in_device_manager(self):
        src = _read("MacAudio/Models/AudioDeviceManager.swift")
        self.assertNotIn("DispatchQueue.main.async", src,
                          "AudioDeviceManager listener should not have double dispatch")


# ---------------------------------------------------------------------------
# Tier 2 — defaultGain constant sync
# ---------------------------------------------------------------------------

class TestDefaultGainConstant(unittest.TestCase):
    """defaultGain must be defined in AudioConstants and used elsewhere."""

    def test_default_gain_in_constants(self):
        src = _read("MacAudio/AudioEngine/AudioConstants.swift")
        self.assertRegex(src, r"defaultGain.*=.*0\.7",
                         "AudioConstants must define defaultGain = 0.7")

    def test_app_state_uses_default_gain(self):
        src = _read("MacAudio/Models/AppState.swift")
        self.assertIn("AudioConstants.defaultGain", src,
                       "AppState must use AudioConstants.defaultGain for initial volume")

    def test_audio_mixer_uses_default_gain(self):
        src = _read("MacAudio/AudioEngine/AudioMixer.swift")
        self.assertIn("AudioConstants.defaultGain", src,
                       "AudioMixer must use AudioConstants.defaultGain for initial gain")


# ---------------------------------------------------------------------------
# Tier 2 — MicCapture callback uses UInt32 (not Float64)
# ---------------------------------------------------------------------------

class TestMicCaptureCallbackType(unittest.TestCase):
    """MicCapture callback must pass channel count as UInt32, not Float64."""

    def test_callback_signature_uses_uint32(self):
        src = _read("MacAudio/AudioEngine/MicCapture.swift")
        self.assertRegex(src, r"UnsafePointer<Float>,\s*UInt32,\s*UInt32",
                         "MicCapture callback must be (UnsafePointer<Float>, UInt32, UInt32)")
        self.assertNotRegex(src, r"UnsafePointer<Float>,\s*UInt32,\s*Float64",
                            "MicCapture callback must NOT pass channel count as Float64")


# ---------------------------------------------------------------------------
# Tier 2 — statusItem optional safety
# ---------------------------------------------------------------------------

class TestStatusItemOptional(unittest.TestCase):
    """statusItem must be NSStatusItem? (not force-unwrapped)."""

    def test_status_item_is_optional(self):
        src = _read("MacAudio/MacAudioApp.swift")
        self.assertRegex(src, r"statusItem:\s*NSStatusItem\?",
                         "statusItem must be NSStatusItem? (not NSStatusItem!)")
        self.assertNotRegex(src, r"statusItem:\s*NSStatusItem!",
                            "statusItem must NOT be implicitly unwrapped")

    def test_build_menu_has_guard_let(self):
        src = _read("MacAudio/MacAudioApp.swift")
        self.assertIn("guard let statusItem", src,
                       "buildMenu() must guard let statusItem for safety")


# ---------------------------------------------------------------------------
# Tier 2 — import Combine at top of file
# ---------------------------------------------------------------------------

class TestImportOrdering(unittest.TestCase):
    """Imports must be at the top of the file, not scattered."""

    def test_combine_import_near_top(self):
        src = _read("MacAudio/MacAudioApp.swift")
        lines = src.split("\n")
        for i, line in enumerate(lines):
            if "import Combine" in line:
                self.assertLess(i, 10,
                                f"import Combine should be near top of file, found at line {i+1}")
                break
        else:
            self.fail("import Combine not found in MacAudioApp.swift")


# ---------------------------------------------------------------------------
# Tier 3 — SharedRingBuffer struct layout assertion
# ---------------------------------------------------------------------------

class TestSharedRingBufferStructAssert(unittest.TestCase):
    """SharedRingBuffer.c must have _Static_assert on buffer offset."""

    def test_static_assert_on_buffer_offset(self):
        src = _read("MacAudioDriver/SharedRingBuffer.c")
        self.assertIn("_Static_assert", src,
                       "SharedRingBuffer.c must have _Static_assert for struct layout")
        self.assertIn("offsetof", src,
                       "Static assert must use offsetof to verify buffer alignment")


# ---------------------------------------------------------------------------
# Tier 3 — extern const sample rates in header
# ---------------------------------------------------------------------------

class TestSampleRatesExternDecl(unittest.TestCase):
    """kSupportedSampleRates must be extern in header, defined in .c file."""

    def test_header_has_extern(self):
        src = _read("MacAudioDriver/MacAudioDriver.h")
        self.assertIn("extern const Float64 kSupportedSampleRates", src,
                       "kSupportedSampleRates must be extern in header")
        # Must NOT be defined with values in the header (that creates duplicate copies)
        self.assertNotIn("{44100", src,
                          "kSupportedSampleRates must NOT be defined with values in header")

    def test_c_file_has_definition(self):
        src = _read("MacAudioDriver/MacAudioDriver.c")
        self.assertRegex(src, r"const\s+Float64\s+kSupportedSampleRates\[",
                         "kSupportedSampleRates must be defined in .c file")


# ---------------------------------------------------------------------------
# Tier 3 — Build config validation
# ---------------------------------------------------------------------------

class TestBuildConfig(unittest.TestCase):
    """project.yml must have proper build configurations."""

    @classmethod
    def setUpClass(cls):
        cls.src = _read("project.yml")

    def test_has_debug_release_configs(self):
        self.assertIn("Debug: debug", self.src)
        self.assertIn("Release: release", self.src)

    def test_debug_has_optimization_none(self):
        self.assertIn('SWIFT_OPTIMIZATION_LEVEL: "-Onone"', self.src)

    def test_release_has_optimization(self):
        self.assertIn('SWIFT_OPTIMIZATION_LEVEL: "-O"', self.src)

    def test_debug_swift_condition(self):
        self.assertIn("SWIFT_ACTIVE_COMPILATION_CONDITIONS", self.src)
        self.assertIn("DEBUG", self.src)

    def test_driver_has_hardened_runtime(self):
        """Both app and driver targets must have ENABLE_HARDENED_RUNTIME."""
        # The string appears at least twice (once per target)
        count = self.src.count("ENABLE_HARDENED_RUNTIME: true")
        self.assertGreaterEqual(count, 2,
                                f"Expected ENABLE_HARDENED_RUNTIME in both targets, found {count}")


# ---------------------------------------------------------------------------
# Tier 3 — Reusable driver log instance
# ---------------------------------------------------------------------------

class TestDriverLogging(unittest.TestCase):
    """Driver must reuse gLog instead of creating os_log_t per call."""

    @classmethod
    def setUpClass(cls):
        cls.drv_c = _read("MacAudioDriver/MacAudioDriver.c")

    def test_gLog_declared(self):
        self.assertRegex(self.drv_c, r"os_log_t\s+gLog",
                         "Driver must declare a static gLog for reuse")

    def test_gLog_used_throughout(self):
        count = self.drv_c.count("os_log(gLog,")
        self.assertGreater(count, 5,
                           f"gLog should be used in many places, found {count} uses")


# ---------------------------------------------------------------------------
# v0.3.0 — DriverInstallError enum
# ---------------------------------------------------------------------------

class TestDriverInstallError(unittest.TestCase):
    """DriverInstaller must define typed errors and return Result."""

    @classmethod
    def setUpClass(cls):
        cls.src = _read("MacAudio/Utilities/DriverInstaller.swift")

    def test_error_enum_exists(self):
        self.assertIn("enum DriverInstallError", self.src,
                       "DriverInstaller must define DriverInstallError enum")

    def test_conforms_to_localized_error(self):
        self.assertIn("LocalizedError", self.src,
                       "DriverInstallError must conform to LocalizedError")

    def test_has_driver_not_found_case(self):
        self.assertIn("driverNotFound", self.src)

    def test_has_user_cancelled_case(self):
        self.assertIn("userCancelled", self.src)

    def test_has_script_failed_case(self):
        self.assertIn("scriptFailed", self.src)

    def test_returns_result_type(self):
        self.assertIn("Result<Void, DriverInstallError>", self.src,
                       "installDriver must return Result<Void, DriverInstallError>")

    def test_detects_user_cancel_error_code(self):
        self.assertIn("-128", self.src,
                       "Must detect NSAppleScript error code -128 (user cancelled)")


# ---------------------------------------------------------------------------
# v0.3.0 — Permission pre-checks
# ---------------------------------------------------------------------------

class TestPermissionPreChecks(unittest.TestCase):
    """AppState must check and expose mic/screen capture permissions."""

    @classmethod
    def setUpClass(cls):
        cls.app_state = _read("MacAudio/Models/AppState.swift")
        cls.app_delegate = _read("MacAudio/MacAudioApp.swift")

    def test_mic_permission_property(self):
        self.assertIn("micPermissionGranted", self.app_state,
                       "AppState must have micPermissionGranted property")

    def test_screen_capture_permission_property(self):
        self.assertIn("screenCapturePermissionGranted", self.app_state,
                       "AppState must have screenCapturePermissionGranted property")

    def test_av_foundation_import(self):
        self.assertIn("import AVFoundation", self.app_state,
                       "AppState must import AVFoundation for mic permission checks")

    def test_checks_mic_authorization_status(self):
        self.assertIn("authorizationStatus", self.app_state,
                       "AppState must check AVCaptureDevice.authorizationStatus")

    def test_checks_screen_capture_preflight(self):
        self.assertIn("CGPreflightScreenCaptureAccess", self.app_state,
                       "AppState must call CGPreflightScreenCaptureAccess")

    def test_request_mic_permission_method(self):
        self.assertIn("func requestMicPermission", self.app_state,
                       "AppState must have requestMicPermission method")

    def test_request_screen_permission_method(self):
        self.assertIn("func requestScreenPermission", self.app_state,
                       "AppState must have requestScreenPermission method")

    def test_request_screen_calls_cg_api(self):
        self.assertIn("CGRequestScreenCaptureAccess", self.app_state,
                       "requestScreenPermission must call CGRequestScreenCaptureAccess")

    def test_delegate_has_permission_actions(self):
        self.assertIn("requestMicPermission", self.app_delegate,
                       "AppDelegate must have requestMicPermission action")
        self.assertIn("requestScreenPermission", self.app_delegate,
                       "AppDelegate must have requestScreenPermission action")

    def test_menu_shows_permission_warnings(self):
        self.assertIn("Grant Access", self.app_delegate,
                       "Menu must show mic permission guidance")
        self.assertIn("Screen Recording", self.app_delegate,
                       "Menu must show screen recording permission guidance")


# ---------------------------------------------------------------------------
# v0.3.0 — Capture status indicator
# ---------------------------------------------------------------------------

class TestCaptureStatusIndicator(unittest.TestCase):
    """AppState must track capture status and menu must show dynamic icon."""

    @classmethod
    def setUpClass(cls):
        cls.app_state = _read("MacAudio/Models/AppState.swift")
        cls.app_delegate = _read("MacAudio/MacAudioApp.swift")

    def test_capture_status_enum(self):
        self.assertIn("enum CaptureStatus", self.app_state,
                       "Must define CaptureStatus enum")

    def test_capture_status_cases(self):
        self.assertIn("stopped", self.app_state)
        self.assertIn("micOnly", self.app_state)
        # 'both' is a common word, so check in context
        self.assertRegex(self.app_state, r"case\s+.*both",
                         "CaptureStatus must have .both case")

    def test_capture_status_property(self):
        self.assertIn("captureStatus", self.app_state,
                       "AppState must have captureStatus property")

    def test_start_sets_capture_status(self):
        self.assertRegex(self.app_state, r"captureStatus\s*=\s*\.both",
                         "startAudio must set captureStatus to .both on success")
        self.assertRegex(self.app_state, r"captureStatus\s*=\s*\.micOnly",
                         "startAudio must set captureStatus to .micOnly on system capture failure")

    def test_stop_resets_capture_status(self):
        self.assertRegex(self.app_state, r"captureStatus\s*=\s*\.stopped",
                         "stopAudio must set captureStatus to .stopped")

    def test_dynamic_menu_bar_icon(self):
        self.assertIn("speaker.wave.2.fill", self.app_delegate,
                       "Menu bar must use speaker.wave.2.fill for both-active state")
        self.assertIn("mic.fill", self.app_delegate,
                       "Menu bar must use mic.fill for mic-only state")

    def test_menu_shows_capture_status(self):
        self.assertIn("Mic: Capturing", self.app_delegate,
                       "Menu must show mic capture status")
        self.assertIn("System Audio: Capturing", self.app_delegate,
                       "Menu must show system audio capture status")


# ---------------------------------------------------------------------------
# v0.3.0 — Volume sliders in menu
# ---------------------------------------------------------------------------

class TestVolumeSliders(unittest.TestCase):
    """Menu must have volume sliders with selective rebuild to avoid focus loss."""

    @classmethod
    def setUpClass(cls):
        cls.app_delegate = _read("MacAudio/MacAudioApp.swift")

    def test_slider_factory_method(self):
        self.assertIn("makeSliderMenuItem", self.app_delegate,
                       "AppDelegate must have makeSliderMenuItem helper")

    def test_uses_nsslider(self):
        self.assertIn("NSSlider", self.app_delegate,
                       "Volume sliders must use NSSlider")

    def test_mic_slider_action(self):
        self.assertIn("micSliderChanged", self.app_delegate,
                       "Must have micSliderChanged action")

    def test_system_slider_action(self):
        self.assertIn("systemSliderChanged", self.app_delegate,
                       "Must have systemSliderChanged action")

    def test_slider_labels(self):
        self.assertIn("Mic Volume", self.app_delegate,
                       "Menu must have Mic Volume slider")
        self.assertIn("System Volume", self.app_delegate,
                       "Menu must have System Volume slider")

    def test_selective_rebuild_not_object_will_change(self):
        """Menu must observe specific properties, not objectWillChange (causes slider focus loss)."""
        self.assertNotIn("objectWillChange", self.app_delegate,
                          "Must NOT use objectWillChange (causes slider focus loss during drag)")
        self.assertIn("Publishers.MergeMany", self.app_delegate,
                       "Must use Publishers.MergeMany for selective property observation")

    def test_volume_not_in_rebuild_publishers(self):
        """micVolume/systemVolume must NOT be in the rebuild publishers."""
        self.assertNotIn("$micVolume", self.app_delegate,
                          "micVolume must NOT trigger menu rebuild")
        self.assertNotIn("$systemVolume", self.app_delegate,
                          "systemVolume must NOT trigger menu rebuild")


if __name__ == "__main__":
    unittest.main()
