// AppleSignInTests.swift - Sign in with Apple's iOS half against a real `cbase serve` and the fake
// Apple, from the simulator: the C client's nonce and state, the credential, the name a new account
// asks for, a taken name asked again, the same identity signing in again, and a link. The server and
// the fake are the host's (make -C c_base/client swift-e2e starts both and passes their URLs in the
// test run's environment, CB_SERVER and CB_MINT); without them the tests that need them are skipped.
import AuthenticationServices
import XCTest
@testable import CbaseAppleExample

@MainActor
final class AppleSignInTests: XCTestCase {
    private var server: URL? { ProcessInfo.processInfo.environment["CB_SERVER"].flatMap(URL.init(string:)) }
    private var mint: URL? { ProcessInfo.processInfo.environment["CB_MINT"].flatMap(URL.init(string:)) }
    private func person() -> String { "p" + String(UInt32.random(in: 0...UInt32.max), radix: 36) }

    func testCredentialFromValues() {
        var n = PersonNameComponents()
        n.givenName = "Ada"
        n.familyName = "Lovelace"
        let c = AppleCredential(identityToken: "a.b.c", authorizationCode: nil, user: "u", email: nil, fullName: n, state: "s")
        XCTAssertEqual(c.suggestedName, "Ada Lovelace")
        XCTAssertEqual(AppleCredential(identityToken: "t", authorizationCode: nil, user: "u", email: nil, fullName: nil, state: nil).suggestedName, "")
    }

    func testPrepareFillsApplesRequest() {
        let s = AppleSignIn(client: CbClient(server: URL(string: "http://127.0.0.1:1")!)) { _, _ in nil }
        let r = ASAuthorizationAppleIDProvider().createRequest()
        s.prepare(r)
        XCTAssertEqual(r.requestedScopes, [.fullName, .email])
        XCTAssertEqual(r.nonce?.count, 64)                               // the raw nonce's SHA-256, hex
        XCTAssertTrue(r.nonce?.allSatisfy { $0.isHexDigit } ?? false)
        XCTAssertEqual(r.state?.count, 22)
    }

    func testBeginIsTheClientsNonceAndState() throws {
        let c = CbClient(server: URL(string: "http://127.0.0.1:1")!)
        let a = c.oidcBegin(), b = c.oidcBegin()
        XCTAssertEqual(a.nonce.count, 64)
        XCTAssertEqual(a.state.count, 22)
        XCTAssertNotEqual(a.nonce, b.nonce)
        XCTAssertNotEqual(a.state, b.state)
    }

    func testNewAccountAsksANameThenSignsInAgain() async throws {
        guard let server, let mint else { throw XCTSkip("no CB_SERVER and CB_MINT: run make -C c_base/client swift-e2e") }
        let who = person()
        let fake = FakeApple(mint: mint, person: who)
        let client = CbClient(server: server)
        var asked: [(String, Bool)] = []
        // A name another account holds first, then a free one.
        let taken = "T" + who.uppercased()
        _ = try await client.signIn(name: taken, password: "correct-horse-1", signUp: true)
        try await client.signOut()
        let s = AppleSignIn(client: client) { suggested, wasTaken in
            asked.append((suggested, wasTaken))
            return asked.count == 1 ? taken : "N" + who
        }
        let b = s.begin()
        let cred = try await fake.credential(nonce: b.nonce, state: b.state)
        XCTAssertEqual(cred.suggestedName, who.prefix(1).uppercased() + who.dropFirst() + " Fake")
        let u = try await s.complete(cred)
        XCTAssertEqual(u?.name, "N" + who.uppercased())
        XCTAssertEqual(asked.count, 2)
        XCTAssertEqual(asked[0].1, false)
        XCTAssertEqual(asked[1].1, true)            // the first name was taken
        let first = try XCTUnwrap(u?.userId)
        // Signed out, and in again with Apple: the same account, no name asked.
        try await client.signOut()
        XCTAssertNil(client.user)
        let b2 = s.begin()
        let u2 = try await s.complete(try await fake.credential(nonce: b2.nonce, state: b2.state))
        XCTAssertEqual(u2?.userId, first)
        XCTAssertEqual(asked.count, 2)
        // The same credential again: Apple's token is good once.
        do {
            _ = try await s.complete(try await fake.credential(nonce: b2.nonce, state: b2.state))
            XCTFail("a second use of the begin went through")
        } catch let e as CbError {
            XCTAssertEqual(e.code, Int(CBC_E_STATE))   // the begin was spent by the session
        }
    }

    func testGivingUpOnTheName() async throws {
        guard let server, let mint else { throw XCTSkip("no CB_SERVER and CB_MINT") }
        let client = CbClient(server: server)
        let s = AppleSignIn(client: client) { _, _ in nil }
        let b = s.begin()
        do {
            _ = try await s.complete(try await FakeApple(mint: mint, person: person()).credential(nonce: b.nonce, state: b.state))
            XCTFail("signed in without a name")
        } catch let e as AppleSignInError {
            XCTAssertEqual(e, .nameNotGiven)
        }
        XCTAssertNil(client.user)
    }

    func testLinkAddsAppleToAPasswordAccount() async throws {
        guard let server, let mint else { throw XCTSkip("no CB_SERVER and CB_MINT") }
        let who = person()
        let client = CbClient(server: server)
        _ = try await client.signIn(name: "L" + who, password: "correct-horse-1", signUp: true)
        let mine = try XCTUnwrap(client.user?.userId)
        let s = AppleSignIn(client: client) { _, _ in XCTFail("a link asks no name"); return nil }
        s.link = true
        let b = s.begin()
        let linked = try await s.complete(try await FakeApple(mint: mint, person: who).credential(nonce: b.nonce, state: b.state))
        XCTAssertNil(linked)
        try await client.signOut()
        s.link = false
        let b2 = s.begin()
        let u = try await s.complete(try await FakeApple(mint: mint, person: who).credential(nonce: b2.nonce, state: b2.state))
        XCTAssertEqual(u?.userId, mine)
    }
}
