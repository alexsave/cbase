// SignInFlowUITests.swift - the example app driven as a person drives it, in the simulator: the
// Sign in with Apple button tapped (SignInWithAppleView's test seam answers it from the fake Apple),
// a name typed for the new account, and the signed-in screen. Against the host's `cbase serve` and
// fake Apple (CB_SERVER, CB_MINT in the test run's environment); skipped without them. A screenshot
// of each step is kept with the result (CB_SHOTS, a directory, keeps them as files too).
import XCTest

final class SignInFlowUITests: XCTestCase {
    private func shot(_ app: XCUIApplication, _ name: String) {
        let s = app.screenshot()
        let a = XCTAttachment(screenshot: s)
        a.name = name
        a.lifetime = .keepAlways
        add(a)
        if let dir = ProcessInfo.processInfo.environment["CB_SHOTS"] {
            try? s.pngRepresentation.write(to: URL(fileURLWithPath: dir).appendingPathComponent("\(name).png"))
        }
    }

    @MainActor
    func testTapSignInPickANameSignedIn() throws {
        let env = ProcessInfo.processInfo.environment
        guard let server = env["CB_SERVER"], let mint = env["CB_MINT"] else { throw XCTSkip("no CB_SERVER and CB_MINT") }
        let person = "u" + String(UInt32.random(in: 0...UInt32.max), radix: 36)
        let app = XCUIApplication()
        app.launchArguments = ["-CbServer", server, "-CbFakeApple", mint, "-CbPerson", person]
        app.launch()
        let button = app.buttons["Sign in with Apple"].firstMatch
        XCTAssertTrue(button.waitForExistence(timeout: 10))
        shot(app, "1-welcome")
        button.tap()
        let field = app.textFields["Name"]
        XCTAssertTrue(field.waitForExistence(timeout: 10))
        XCTAssertEqual(field.value as? String, person.prefix(1).uppercased() + person.dropFirst())   // Apple's given name, suggested
        shot(app, "2-pick-a-name")
        app.buttons["Continue"].tap()
        XCTAssertTrue(app.staticTexts["Signed in"].waitForExistence(timeout: 10))
        let named = NSPredicate(format: "label CONTAINS %@", person.uppercased())
        XCTAssertTrue(app.descendants(matching: .any).matching(named).firstMatch.exists)
        shot(app, "3-signed-in")
        // Signed out: the button again.
        app.buttons["Sign out"].tap()
        XCTAssertTrue(button.waitForExistence(timeout: 10))
    }
}
