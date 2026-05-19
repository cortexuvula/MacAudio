import Foundation

extension UserDefaults {
    /// Reads a Bool that may not be present in defaults.
    ///
    /// `UserDefaults.bool(forKey:)` returns `false` for missing keys, which
    /// conflates "user opted out" with "cfprefsd hasn't loaded our domain yet"
    /// — the v0.6.4 bug where Auto-Start Capturing silently turned itself off
    /// after login. This helper returns `defaultIfMissing` when the key has
    /// never been written, so callers can preserve their in-memory default
    /// instead of being clobbered to `false`.
    func boolIfPresent(forKey key: String, default defaultIfMissing: Bool) -> Bool {
        guard object(forKey: key) != nil else { return defaultIfMissing }
        return bool(forKey: key)
    }
}
