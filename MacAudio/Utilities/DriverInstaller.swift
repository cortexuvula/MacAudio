import Foundation

enum DriverInstaller {
    static let driverBundleName = "MacAudioDriver.driver"
    static let halPluginDir = "/Library/Audio/Plug-Ins/HAL"

    static func isDriverInstalled() -> Bool {
        FileManager.default.fileExists(
            atPath: "\(halPluginDir)/\(driverBundleName)")
    }

    static func installDriver(completion: @escaping (Bool) -> Void) {
        guard let driverSource = Bundle.main.path(
            forResource: "MacAudioDriver", ofType: "driver") else {
            completion(false)
            return
        }

        let script = """
        do shell script "rm -rf '\(halPluginDir)/\(driverBundleName)' ; \
        cp -R '\(driverSource)' '\(halPluginDir)/' && \
        xattr -rc '\(halPluginDir)/\(driverBundleName)' && \
        killall coreaudiod" \
        with administrator privileges
        """

        DispatchQueue.global(qos: .userInitiated).async {
            var error: NSDictionary?
            NSAppleScript(source: script)?.executeAndReturnError(&error)
            let success = error == nil
            DispatchQueue.main.async {
                completion(success)
            }
        }
    }

}
