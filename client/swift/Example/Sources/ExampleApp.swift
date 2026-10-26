// ExampleApp.swift - one screen: the Sign in with Apple button, a name for a new account, and the
// signed-in user. Launch arguments (the simulator run, docs/AUTH_APPLE.md):
//   -CbServer <url>     the cbase serve (default http://127.0.0.1:8080)
//   -CbFakeApple <url>  the fake Apple's mint: the button's tap uses it instead of Apple
//   -CbPerson <who>     the fake's person (default ada)
//   -CbAuto 1           tap the button and take Apple's suggested name, for an unattended run
import SwiftUI

@main
struct ExampleApp: App {
    var body: some Scene {
        WindowGroup { RootView() }
    }
}

@MainActor
final class Model: ObservableObject {
    @Published var user: CbUser?
    @Published var error = ""
    @Published var asking: (suggested: String, taken: Bool)?
    private var answer: CheckedContinuation<String?, Never>?
    let client: CbClient
    let signIn: AppleSignIn
    let fake: FakeApple?
    let auto: Bool

    init() {
        let d = UserDefaults.standard
        client = CbClient(server: URL(string: d.string(forKey: "CbServer") ?? "http://127.0.0.1:8080")!)
        fake = d.string(forKey: "CbFakeApple").flatMap(URL.init(string:)).map { FakeApple(mint: $0, person: d.string(forKey: "CbPerson") ?? "ada") }
        auto = d.bool(forKey: "CbAuto")
        var ask: ((String, Bool) async -> String?)!
        signIn = AppleSignIn(client: client, askName: { s, t in await ask(s, t) })
        ask = { [unowned self] s, t in await self.ask(s, t) }
        user = client.user
    }

    func ask(_ suggested: String, _ taken: Bool) async -> String? {
        if auto && !taken { return suggested.components(separatedBy: " ").first }
        return await withCheckedContinuation { k in
            answer = k
            asking = (suggested, taken)
        }
    }

    // What a person reads for a refusal.
    static func say(_ e: Error) -> String {
        switch e {
        case AppleSignInError.nameNotGiven: return "Sign-in cancelled."
        case let c as CbError where c.isAccount(CBW_ACCOUNT_E_TOO_MANY): return "Too many attempts. Wait a minute and try again."
        case let c as CbError where c.isAccount(CBW_ACCOUNT_E_BAD_IDENTITY): return "Apple's sign-in could not be checked. Try again."
        case let c as CbError where c.status == 0: return "The server could not be reached."
        default: return "Sign-in failed (\(e))."
        }
    }

    func answered(_ name: String?) {
        asking = nil
        answer?.resume(returning: name)
        answer = nil
    }
}

struct RootView: View {
    @StateObject private var m = Model()
    @State private var name = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            VStack(alignment: .leading, spacing: 4) {
                Text("c_base").font(.largeTitle.bold())
                Text(m.fake == nil ? "against Apple" : "against the fake Apple (no Apple ID involved)")
                    .font(.footnote).foregroundStyle(.secondary)
            }
            if let u = m.user {
                VStack(alignment: .leading, spacing: 10) {
                    Text("Signed in").font(.title2.bold())
                    LabeledContent("Name", value: u.name)
                    LabeledContent("User id") { Text(u.userId).font(.caption.monospaced()).multilineTextAlignment(.trailing) }
                    LabeledContent("Session ends", value: u.expiresAt.formatted(date: .omitted, time: .shortened))
                    Button("Sign out") {
                        Task { try? await m.client.signOut(); m.user = nil }
                    }
                    .buttonStyle(.bordered)
                    .padding(.top, 4)
                }
                .padding(20)
                .background(.background.secondary, in: RoundedRectangle(cornerRadius: 14))
            } else if let a = m.asking {
                VStack(alignment: .leading, spacing: 12) {
                    Text("Pick a name").font(.title2.bold())
                    Text(a.taken ? "That name is taken. Try another." : "Your account is new. Choose the name people will see.")
                        .foregroundStyle(.secondary)
                    TextField("Name", text: $name).textFieldStyle(.roundedBorder).onAppear { name = a.suggested.components(separatedBy: " ").first ?? "" }
                    HStack {
                        Button("Continue") { m.answered(name) }.buttonStyle(.borderedProminent)
                        Button("Cancel") { m.answered(nil) }
                    }
                }
                .padding(20)
                .background(.background.secondary, in: RoundedRectangle(cornerRadius: 14))
            } else {
                VStack(alignment: .leading, spacing: 14) {
                    Text("Welcome").font(.title2.bold())
                    Text("One account for every app on this server.").foregroundStyle(.secondary)
                    SignInWithAppleView(signIn: m.signIn, fakeCredential: m.fake.map { f in { @MainActor n, s in try await f.credential(nonce: n, state: s) } }) { r in
                        switch r {
                        case .success(let u): m.user = u
                        case .failure(let e): m.error = Model.say(e)
                        }
                    }
                }
                .padding(20)
                .background(.background.secondary, in: RoundedRectangle(cornerRadius: 14))
            }
            if !m.error.isEmpty { Text(m.error).foregroundStyle(.red).font(.footnote) }
            Spacer()
        }
        .padding(.horizontal, 16)
        .padding(.top, 24)
        .task {
            guard m.auto, m.user == nil, let f = m.fake else { return }
            let b = m.signIn.begin()
            do {
                let cred = try await f.credential(nonce: b.nonce, state: b.state)
                m.user = try await m.signIn.complete(cred)
            } catch { m.error = Model.say(error) }
        }
    }
}
