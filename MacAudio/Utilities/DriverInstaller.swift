import Foundation

enum DriverInstallError: LocalizedError {
    case driverNotFound
    case userCancelled
    case scriptFailed(String)

    var errorDescription: String? {
        switch self {
        case .driverNotFound:
            return "Audio driver bundle not found in app resources"
        case .userCancelled:
            return "Driver installation was cancelled"
        case .scriptFailed(let detail):
            return "Driver installation failed: \(detail)"
        }
    }
}

enum DriverInstaller {
    static let driverBundleName = "MacAudioDriver.driver"
    static let halPluginDir = "/Library/Audio/Plug-Ins/HAL"

    static func isDriverInstalled() -> Bool {
        FileManager.default.fileExists(
            atPath: "\(halPluginDir)/\(driverBundleName)")
    }

    static func installDriver(completion: @escaping (Result<Void, DriverInstallError>) -> Void) {
        guard let driverSource = Bundle.main.path(
            forResource: "MacAudioDriver", ofType: "driver") else {
            completion(.failure(.driverNotFound))
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

            let result: Result<Void, DriverInstallError>
            if let error {
                let errorNumber = error[NSAppleScript.errorNumber] as? Int
                if errorNumber == -128 {
                    result = .failure(.userCancelled)
                } else {
                    let message = error[NSAppleScript.errorMessage] as? String ?? "Unknown error"
                    result = .failure(.scriptFailed(message))
                }
            } else {
                result = .success(())
            }

            DispatchQueue.main.async {
                completion(result)
            }
        }
    }

}
