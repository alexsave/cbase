// FakeApple.swift - the example's test seam (SignInWithAppleView's fakeCredential): a credential
// from c_base's fake Apple (c_base/core/test/fake_apple.c, its /mint), for this sign-in's nonce and
// state, so the simulator runs the whole flow with no Apple ID. Its answer is Apple's own JSON shape;
// it is read here, in the example, and nowhere in the client.
import Foundation

struct FakeApple: Sendable {
    let mint: URL
    var person = "ada"
    var clientId = "com.example.app"

    private struct Answer: Decodable {
        struct Auth: Decodable { let id_token: String; let code: String?; let state: String? }
        struct Name: Decodable { let firstName: String?; let lastName: String? }
        struct User: Decodable { let name: Name?; let email: String? }
        let authorization: Auth
        let user: User?
    }

    func credential(nonce: String, state: String) async throws -> AppleCredential {
        var u = URLComponents(url: mint.appendingPathComponent("mint"), resolvingAgainstBaseURL: false)!
        u.queryItems = [URLQueryItem(name: "aud", value: clientId), URLQueryItem(name: "nonce", value: nonce),
                        URLQueryItem(name: "state", value: state), URLQueryItem(name: "person", value: person)]
        let (data, resp) = try await URLSession.shared.data(from: u.url!)
        guard (resp as? HTTPURLResponse)?.statusCode == 200 else { throw URLError(.badServerResponse) }
        let a = try JSONDecoder().decode(Answer.self, from: data)
        var name: PersonNameComponents?
        if let n = a.user?.name {
            var c = PersonNameComponents()
            c.givenName = n.firstName
            c.familyName = n.lastName
            name = c
        }
        return AppleCredential(identityToken: a.authorization.id_token, authorizationCode: a.authorization.code,
                               user: "001234.\(person).fake", email: a.user?.email, fullName: name, state: a.authorization.state)
    }
}
