// SignInWithAppleView.swift - Apple's own Sign in with Apple button (AuthenticationServices'
// SignInWithAppleButton) wired to AppleSignIn: Apple draws the button and runs the sheet; the
// answer goes to the c_base client. Black on a light screen, white on a dark one (Apple's rule).
//
// THE TEST SEAM. A simulator cannot finish a real Apple sign-in without an Apple ID. Given
// `fakeCredential`, the same button is drawn, and a tap asks that function for the credential
// (for this sign-in's nonce and state) instead of Apple: the rest of the flow is the real one.
// Nothing sets it in a shipped build; the example app sets it from a launch argument.
import AuthenticationServices
import SwiftUI

public struct SignInWithAppleView: View {
    @Environment(\.colorScheme) private var scheme
    private let signIn: AppleSignIn
    private let onResult: (Result<CbUser?, Error>) -> Void
    private let fakeCredential: (@MainActor (_ nonce: String, _ state: String) async throws -> AppleCredential)?
    @State private var busy = false

    public init(signIn: AppleSignIn,
                fakeCredential: (@MainActor (_ nonce: String, _ state: String) async throws -> AppleCredential)? = nil,
                onResult: @escaping (Result<CbUser?, Error>) -> Void) {
        self.signIn = signIn
        self.fakeCredential = fakeCredential
        self.onResult = onResult
    }

    public var body: some View {
        ZStack {
            SignInWithAppleButton(signIn.link ? .continue : .signIn) { request in
                signIn.prepare(request)
            } onCompletion: { result in
                switch result {
                case .success(let auth):
                    guard let c = auth.credential as? ASAuthorizationAppleIDCredential, let cred = AppleCredential(c) else { return }
                    finish(cred)
                case .failure(let e):
                    if (e as? ASAuthorizationError)?.code != .canceled { onResult(.failure(e)) }
                }
            }
            .signInWithAppleButtonStyle(scheme == .dark ? .white : .black)
            if let fake = fakeCredential {
                Color.clear
                    .contentShape(Rectangle())
                    .onTapGesture {
                        guard !busy else { return }
                        let b = signIn.begin()
                        Task { @MainActor in
                            do { finish(try await fake(b.nonce, b.state)) } catch { onResult(.failure(error)) }
                        }
                    }
                    .accessibilityElement()
                    .accessibilityLabel("Sign in with Apple")
                    .accessibilityAddTraits(.isButton)
            }
        }
        .frame(height: 50)
        .opacity(busy ? 0.6 : 1)
        .disabled(busy)
    }

    private func finish(_ cred: AppleCredential) {
        busy = true
        Task { @MainActor in
            defer { busy = false }
            do { onResult(.success(try await signIn.complete(cred))) } catch { onResult(.failure(error)) }
        }
    }
}
