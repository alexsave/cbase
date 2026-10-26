// AppleSignIn.swift - Sign in with Apple's answer handed to the c_base client, with no UIKit: the
// request is prepared here (scopes, the nonce and state the C client made), the credential is
// turned into a plain value, and the value goes to the server through CbClient. A new account's
// name is asked of the app (askName) and the same answer is sent again with it.
import AuthenticationServices
import Foundation

// What Apple's credential says, as a value a test can make without Apple.
public struct AppleCredential: Equatable, Sendable {
    public var identityToken: String
    public var authorizationCode: String?
    public var user: String                  // Apple's stable user id (the token's sub)
    public var email: String?                // the first sign-in's only
    public var fullName: PersonNameComponents?   // the first sign-in's only
    public var state: String?

    public init(identityToken: String, authorizationCode: String?, user: String, email: String?,
                fullName: PersonNameComponents?, state: String?) {
        self.identityToken = identityToken
        self.authorizationCode = authorizationCode
        self.user = user
        self.email = email
        self.fullName = fullName
        self.state = state
    }

    // nil when Apple gave no identity token (it always does for a completed sign-in).
    public init?(_ c: ASAuthorizationAppleIDCredential) {
        guard let t = c.identityToken.flatMap({ String(data: $0, encoding: .utf8) }) else { return nil }
        self.init(identityToken: t, authorizationCode: c.authorizationCode.flatMap { String(data: $0, encoding: .utf8) },
                  user: c.user, email: c.email, fullName: c.fullName, state: c.state)
    }

    // Apple's name of the person, as a suggestion for an account's name ("" when none came).
    public var suggestedName: String {
        guard let n = fullName else { return "" }
        return PersonNameComponentsFormatter.localizedString(from: n, style: .default, options: [])
    }
}

public enum AppleSignInError: Error, Equatable {
    case nameNotGiven       // askName answered nothing: the person gave up
}

@MainActor
public final class AppleSignIn {
    public let client: CbClient
    // A new account's name: Apple's suggestion, and whether the last one was taken. nil gives up.
    public var askName: (_ suggested: String, _ taken: Bool) async -> String?
    public var link = false

    public init(client: CbClient, askName: @escaping (_ suggested: String, _ taken: Bool) async -> String?) {
        self.client = client
        self.askName = askName
    }

    // A fresh begin: the nonce (hashed) and the state for Apple's request.
    public func begin() -> (nonce: String, state: String) { client.oidcBegin() }

    // Apple's request, filled: the person's name and email, and this sign-in's nonce and state.
    public func prepare(_ request: ASAuthorizationAppleIDRequest) {
        let b = begin()
        request.requestedScopes = [.fullName, .email]
        request.nonce = b.nonce
        request.state = b.state
    }

    // The credential to the server: the signed-in user (nil for a link). Throws the server's refusal
    // (CbError), or AppleSignInError.nameNotGiven.
    @discardableResult
    public func complete(_ cred: AppleCredential) async throws -> CbUser? {
        var name: String?
        var taken = false
        while true {
            do {
                try await client.oidcSignIn(idp: CBW_IDP_APPLE, state: cred.state ?? "", idToken: cred.identityToken,
                                            code: cred.authorizationCode, name: name, link: link)
                return link ? nil : client.user
            } catch let e as CbError where e.isAccount(CBW_ACCOUNT_E_NEEDS_NAME) || (name != nil && e.isAccount(CBW_ACCOUNT_E_EXISTS)) {
                taken = e.isAccount(CBW_ACCOUNT_E_EXISTS)
                guard let n = await askName(cred.suggestedName, taken)?.trimmingCharacters(in: .whitespaces), !n.isEmpty else {
                    throw AppleSignInError.nameNotGiven
                }
                name = n
            }
        }
    }
}
