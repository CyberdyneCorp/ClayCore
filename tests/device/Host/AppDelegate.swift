// An empty app that exists only to host the test bundle.
//
// XCTest on a device destination requires a host application — there is no
// hostless mode there — so this is the smallest thing that satisfies that
// requirement. It deliberately links no claycore: the test bundle links the
// xcframework itself, keeping exactly one copy of the static library in the
// graph.
//
// IT ADOPTS THE SCENE LIFECYCLE, and that is not a modernisation. An app
// linked against the iOS 26 or later SDK that has no `UIApplicationSceneManifest`
// is terminated during launch, before the test runner connects: the console
// says `_UIApplicationEvaluateRuntimeIssueForNoSceneLifecycleAdoption` and
// xcodebuild reports "Early unexpected exit, operation never finished
// bootstrapping" with no test having run. The reference iPad moved to iOS 27.0
// on 2026-09-18, which brought the 27.0 SDK with it, and session 1/7 of the
// gate died 12 seconds in for this reason — with no thermal event, which is
// what tells it apart from the heat kill the gate skill describes.

import UIKit

@main
final class AppDelegate: UIResponder, UIApplicationDelegate {
    func application(
        _ application: UIApplication,
        configurationForConnecting connectingSceneSession: UISceneSession,
        options: UIScene.ConnectionOptions
    ) -> UISceneConfiguration {
        // Named to match the one configuration in Info.plist's scene manifest;
        // a mismatch here is another launch-time termination.
        UISceneConfiguration(
            name: "Default Configuration",
            sessionRole: connectingSceneSession.role
        )
    }
}

final class SceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?

    func scene(
        _ scene: UIScene,
        willConnectTo session: UISceneSession,
        options connectionOptions: UIScene.ConnectionOptions
    ) {
        guard let windowScene = scene as? UIWindowScene else { return }
        let window = UIWindow(windowScene: windowScene)
        window.rootViewController = UIViewController()
        window.rootViewController?.view.backgroundColor = .systemBackground
        window.makeKeyAndVisible()
        self.window = window
    }
}
