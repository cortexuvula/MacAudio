import XCTest
@testable import MacAudio

/// Regression coverage for v0.6.4 — at login, cfprefsd may not have loaded our
/// domain yet, and `UserDefaults.bool(forKey:)` returns `false` for missing
/// keys. Auto-Start Capturing was silently disabling itself on every login.
/// The `boolIfPresent` helper distinguishes missing from explicit-false.
final class UserDefaultsExtensionsTests: XCTestCase {
    private var suiteName: String!
    private var defaults: UserDefaults!

    override func setUp() {
        super.setUp()
        suiteName = "com.macaudio.tests.\(UUID().uuidString)"
        defaults = UserDefaults(suiteName: suiteName)
    }

    override func tearDown() {
        defaults.removePersistentDomain(forName: suiteName)
        defaults = nil
        suiteName = nil
        super.tearDown()
    }

    func test_boolIfPresent_returns_default_when_key_is_missing() {
        // No write happened; reading the key in standard .bool would return
        // false. boolIfPresent must return the provided default instead.
        XCTAssertTrue(defaults.boolIfPresent(forKey: "neverWritten", default: true))
        XCTAssertFalse(defaults.boolIfPresent(forKey: "neverWritten", default: false))
    }

    func test_boolIfPresent_returns_stored_true_when_key_was_set_true() {
        defaults.set(true, forKey: "key")
        XCTAssertTrue(defaults.boolIfPresent(forKey: "key", default: false))
    }

    func test_boolIfPresent_returns_stored_false_when_key_was_set_false() {
        // The key was *explicitly* written false. boolIfPresent must
        // honor that and return false even when default is true — this
        // is what distinguishes "user opted out" from "missing".
        defaults.set(false, forKey: "key")
        XCTAssertFalse(defaults.boolIfPresent(forKey: "key", default: true))
    }

    func test_boolIfPresent_after_remove_falls_back_to_default() {
        defaults.set(true, forKey: "key")
        defaults.removeObject(forKey: "key")
        XCTAssertFalse(defaults.boolIfPresent(forKey: "key", default: false))
        XCTAssertTrue(defaults.boolIfPresent(forKey: "key", default: true))
    }
}
