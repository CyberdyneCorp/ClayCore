// An empty app that exists only to host the test bundle.
//
// XCTest on a device destination requires a host application — there is no
// hostless mode there — so this is the smallest thing that satisfies that
// requirement. It deliberately links no claycore: the test bundle links the
// xcframework itself, keeping exactly one copy of the static library in the
// graph.

import UIKit

@main
final class AppDelegate: UIResponder, UIApplicationDelegate {
    var window: UIWindow?

    func application(
        _ application: UIApplication,
        didFinishLaunchingWithOptions launchOptions:
            [UIApplication.LaunchOptionsKey: Any]? = nil
    ) -> Bool {
        let window = UIWindow(frame: UIScreen.main.bounds)
        window.rootViewController = UIViewController()
        window.rootViewController?.view.backgroundColor = .systemBackground
        window.makeKeyAndVisible()
        self.window = window
        return true
    }
}
