// gmstream iPadOS client: the macOS client's stream (../apple, Session) in a
// UIKit app. Touch, hardware keyboard, mouse and trackpad, and game
// controllers go to the host; settings are a three-finger tap away.
//
// Launch arguments are the macOS client's (Options), e.g. through
// `devicectl device process launch ... -- --mock`.

import UIKit

@main final class AppDelegate: UIResponder, UIApplicationDelegate {
    func application(_ a: UIApplication, configurationForConnecting s: UISceneSession,
                     options: UIScene.ConnectionOptions) -> UISceneConfiguration {
        let c = UISceneConfiguration(name: nil, sessionRole: s.role)
        c.delegateClass = SceneDelegate.self
        return c
    }
}

final class SceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?
    private var stream: StreamViewController? { window?.rootViewController as? StreamViewController }

    func scene(_ scene: UIScene, willConnectTo s: UISceneSession, options: UIScene.ConnectionOptions) {
        guard let ws = scene as? UIWindowScene else { return }
        let w = UIWindow(windowScene: ws)
        w.rootViewController = StreamViewController()
        w.makeKeyAndVisible()
        window = w
    }

    // No GPU work in the background: the stream stops there and reconnects
    // when the app comes back.
    func sceneWillEnterForeground(_ scene: UIScene) { stream?.start() }
    func sceneDidEnterBackground(_ scene: UIScene) { stream?.stop() }
    func sceneWillResignActive(_ scene: UIScene) { stream?.session.releaseAll() }
}
