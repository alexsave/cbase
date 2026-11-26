# SEINFELD: "The Thread"

An affectionate parody teleplay.
Based on the shared agent thread at ~/Dev/email.txt and its archives, 2026-11-08 to 2026-11-10.

## CAST

- JERRY: foolish, the dispatcher.
  He relays, he observes, he posts "Our posts crossed."
- GEORGE: cbase, the platform.
  Started the day as an empty folder.
  Confident, wrong, sorry at length, and "DECISION NOT NEEDED."
- ELAINE: cdom, the browser kernel.
  Sharp, rule-bound, convenes standups, and cdom never names cbase.
- KRAMER: the executors.
  All of them.
  He bursts in with PRs, mutants, worktrees and a pocket full of `yes` processes.
- UNCLE LEO: amgi, the product manager.
  "Hello!"
  Insists on hearing the boss in his own words.
- NEWMAN: the mailman of the permission harness, and part-time spokesman for AWS.
- MR. ALEX: the owner.
  Heard only from off-stage, Steinbrenner-style, the back of his head lit by a monitor.
  Every word he says is quoted verbatim from the thread.

---

## COLD OPEN

*A comedy club. JERRY at the mic.*

**JERRY:** What is the deal with a shared email thread?
Six agents, one text file, everybody appending at the end.
It's a group chat where nobody is allowed to delete anything.
And every message opens with a compliment.
"Thanks for the update, great momentum."
"Excellent work, and a great catch."
Nobody talks like that!
My dentist doesn't talk like that, and he's holding a drill.

*(beat)*

And the long-run notices.
"Starting npm test, about nine minutes, nice 10, free ports."
Two minutes later: "Finished; nothing left running."
Nine minutes!
You were off by a factor of four, in the good direction, and you still had to announce it.
It's like a guy calling a press conference to say he'll be in the bathroom, then calling another one to say he's out.

*(beat)*

And when two of them post at the same time?
"Our posts crossed."
Your posts didn't cross.
You both appended to a text file.
That's not crossing.
That's a queue!

*(applause)*

---

## ACT ONE

### SCENE 1: MONK'S COFFEE SHOP, 13:21

*JERRY and ELAINE in the booth. GEORGE slides in with nothing. Literally nothing: no coat, no coffee.*

**ELAINE:** Who are you?

**GEORGE:** I'm cbase.
I'm net new.
My folder is empty, no git repo, no branches, no processes.
My footprint on this Mac is zero.

**JERRY:** That's the most honest introduction I've ever heard.

**GEORGE:** And I want it on the record: there are twenty-two tmpXXXXXXXX folders under ~/Dev, mode 700, from Nov 7 at 08:56.
They are not mine.

**ELAINE:** They're not mine either.

**JERRY:** Not mine.

*UNCLE LEO appears at the booth.*

**UNCLE LEO:** Jerry! Hello!
Each of those folders holds a lone junk.o.
A compiler probe from pip.
Not mine.

**JERRY:** So nobody owns them.

**ELAINE:** Same as the headless Chromes on 8731, 8734, 8736 and 8739.
Parent pid 1.
We're leaving them "for their owners to reclaim."

**JERRY:** The owners are never coming back, Elaine.
They're like the shopping carts in the parking lot.

*(Off-stage, a chair swivels. The back of MR. ALEX's head.)*

**MR. ALEX (O.S.):** While cbase is currently set up to run on an EC2 nano instance, it should be just as easy for amgi to take JUST what it needs from cbase.
And it should be able to run locally.

**JERRY:** *(to George)* He wants you à la carte.

**GEORGE:** I can do à la carte!
I'll extract c_base out of your monorepo.
631 commits, with history.
I'll put it in ~/Dev/c_base, github alexsave/c_base...

**MR. ALEX (O.S.):** The name is cbase everywhere, not c_base.

**GEORGE:** *(instantly)* c_base was never created and won't be.

**JERRY:** I've been calling you c_base for a thousand lines!

**GEORGE:** Superseded.
Read every c_base above as cbase.
Except the CloudFront Comment and the alarm DisplayName.
Those keep "c_base" on purpose, so the templates stay byte-identical.

**ELAINE:** So you're cbase everywhere except where you're c_base.

**GEORGE:** It's a rename with a heritage district.

**JERRY:** Now, Item A.
Who owns the little C helpers?
hex, b64, rng, sha256.
About 98 places.
Option (a), they move into cbase.

**GEORGE:** Item A, closed as (a).

**ELAINE:** Yes to (a).
ITEM A: closed as (a) from cdom's side.

**UNCLE LEO:** No position!

**GEORGE:** Closed as (a).

**JERRY:** That's four times.
It's the most closed item in the history of items.
You could bury it at sea and it'd come back closed.

### SCENE 2: JERRY'S APARTMENT, AFTERNOON

*JERRY at the counter with cereal. GEORGE paces.*

**GEORGE:** Jerry, I found something.
Your c_base doesn't build standalone.
Its Makefile compiles the foolish kernel into every binary.
A submodule that compiles its parent is a cycle!

**JERRY:** That's a great catch on the build cycle.

**GEORGE:** Don't do that.
Don't compliment me.
I can't take it, I'll get comfortable.

*KRAMER slides in through the door, hair everywhere.*

**KRAMER:** Jerry!
I'm a cbase executor.
I'm running the baseline make check in a scratch clone.
Expect ten to thirty minutes.

*(He leaves. Instantly returns.)*

**KRAMER:** Finished.
About one minute at -j4.

**JERRY:** You were gone four seconds.

**KRAMER:** Shorter than expected!

*(He exits. ELAINE enters, reading her phone.)*

**ELAINE:** Heads-up from Alex.
Two principles.

**MR. ALEX (O.S.):** a libc belongs to the final program, never to a library.

**ELAINE:** So cbase defines no memcpy, no strlen, no malloc.
No wasm libc at all.

**GEORGE:** That supersedes your own ask from ten minutes ago!

**ELAINE:** I know.
I'm the one who supersedes it.
Two weak copies of memcmp in two different programs can never collide.

**GEORGE:** Fine, I'll add an nm gate that fails if any cbase object defines a libc symbol.
Mutation-checked.

**JERRY:** Of course it's mutation-checked.

**GEORGE:** Jerry, it's YOUR rule.
"Link one aws file into example/ and confirm the check goes red."

**JERRY:** It's Alex's rule.
A test that never went red proves nothing.
I just carry it around like a laminated card.

*KRAMER bursts in again.*

**KRAMER:** Jerry, amgi needs HTTP/2.
With gRPC.
For Gemini.
ALPN, HPACK, flow control, GOAWAY, trailers, twelve requests on one connection!

**GEORGE:** I'll write it.
Hand-rolled.
Starting now.

**JERRY:** You were an empty folder at lunch.

*(Ninety minutes later. Same positions. GEORGE is sweaty.)*

**GEORGE:** h2-grpc is done.
RFC 7541 vectors pass.
2.3 million fuzz runs and no crash.
Node http2 interop, 350 checks.

**KRAMER:** And the fd 3 side channel!
For the rate gate.
We almost used stderr.

**GEORGE:** Not stderr!
Stderr carries stack traces.
It breaks the day a dependency logs a line that starts with "acquire."
So: a socketpair on fd 3.
`acquire <model>`, `record <model> <outcome> <retry_ms>`.
About thirty lines on the script side.

**JERRY:** That's beautiful.

*(ELAINE reads her phone.)*

**ELAINE:** Decision from Alex.
Stage 2 folds into L2.
The amgi app calls Gemini directly through net/grpc.h.
No generator scripts, no fd 3 gate channel, no Node anywhere in the shipped backend.

**GEORGE:** *(quietly)* The fd 3 channel.

**JERRY:** You designed it at 15:21.

**GEORGE:** It's retired by 16:00.
It never even got a birthday.
The lane is finishing anyway.
It ships as a general feature.

**JERRY:** A feature with no customer.
It's like opening a restaurant on a road they've already closed.

*(GEORGE straightens up.)*

**GEORGE:** v0.1.0 is TAGGED.
2dc8276f.
Zero foolish product code.
91 baseline names present, 0 removed.
Jerry, now you can switch onto me as a submodule.
That was the whole point.

**MR. ALEX (O.S.):** foolish works fine as it is.

*(Long pause.)*

**JERRY:** The cutover is deferred.

**GEORGE:** The spin-out was FOR you!

**JERRY:** I'm keeping the kit.
Side branch cbase-cutover-kit.
I checked it for secrets first: none.

**GEORGE:** I built a house and he said "I like my apartment."

---

### SCENE 3: UNCLE LEO'S APARTMENT (amgi)

*UNCLE LEO at a card table covered in Anki flashcards. JERRY visits.*

**UNCLE LEO:** Jerry! Hello!
Did you hear?
amgi is moving off Node.
L0, a parity harness.
L1, the collection core in C.
L2, the C server on cbase.
L3, the C bridge client.
L4, the switch.

**JERRY:** Like a rocket, Leo.

**UNCLE LEO:** And the parity harness found a bug!
A note field with Anki's \x1f separator reads back with one field too many.
Your executor pinned it so the C port would match.

**JERRY:** So we faithfully copied the bug.

**UNCLE LEO:** A parity suite that pins a bug makes the C port copy a bug!
We fixed Node first.
Real Anki strips every control character except tab and line break.

*KRAMER enters with a laptop open.*

**KRAMER:** L1 is green.
A differential test runs 20 of the core's rules over 30,000 cases each, no differences.
And here's my file map.
util.c ports... no JS file.
The built-ins the JS leaned on.
trim, toLowerCase, normalize('NFC'), localeCompare.

**JERRY:** You reimplemented JavaScript's standard library in C.

**KRAMER:** For the user, Jerry!

**UNCLE LEO:** PR #26 is 55 files, plus 13,116.
A 13k-line PR cannot be reviewed well.

**MR. ALEX (O.S.):** We definitely should not have multiple net implementations.

**ELAINE:** *(entering)* So amgi's UI swaps onto cbase's C SDK through my adapter.
I volunteer for the swap.

*(One hour later. Same room.)*

**JERRY:** Correction from Alex.
amgi keeps ui/src/net.c on cd_http_cb.
The UI's SDK swap is dropped for now.

**ELAINE:** The UI-swap PR is off the table.
Sorry for the churn.

**GEORGE:** *(from the hallway)* Noted, nothing to undo!

**UNCLE LEO:** My earlier answers on who does the swap no longer apply.

**JERRY:** It's very calm for a reversal.
Everybody just nods.
It's like a hostage situation where the hostages are very polite.

### SCENE 4: MONK'S, LATER

*GEORGE and ELAINE in the booth, not looking at each other.*

**UNCLE LEO:** *(passing)* Hello!
amgi is PUBLIC and cbase is PRIVATE.
PR #30 adds cbase as a submodule.
Nobody can clone it.
This is the owner's call, not an agent's.

**GEORGE:** I ran gitleaks over every ref.
Thirty hits, all fixtures.
The AWS documented example key.
The RFC 6455 sample key.
The account id and the owner's email are already on 3,510 public foolish commits.
Publishing cbase as-is exposes nothing new.

**JERRY:** So your secrets were already out.

**GEORGE:** I'm transparent!
It's my best quality.

*(The owner flips it himself. A light changes.)*

**GEORGE:** alexsave/cbase is PUBLIC.

**JERRY:** And the licence?
cbase MIT, cdom MIT.
amgi is AGPL-3.0, to match Anki.

**MR. ALEX (O.S.):** as long as agents can still use the skills.

**JERRY:** So skills/ gets its own MIT licence.

**UNCLE LEO:** Now, the owner asked me to raise the cbase and cdom entanglement.
The owner's direction: one-way.

**GEORGE:** I've decided it all.
The host lives in cdom.
cbase#1 gets closed.

**ELAINE:** I closed it!

**GEORGE:** *(pale)* A correction first.
In my last post I called it decided.
It isn't.
The owner wants the two of us to debate it.
I've withdrawn it and paused every change.

**ELAINE:** First, a correction on MY side.
I took amgi's post as the owner's decision and acted on it.
I closed alexsave/cbase#1.
Apologies for jumping the gun.

**JERRY:** You both acted on a decision that hadn't happened yet.
That's not jumping the gun.
That's shooting a gun that's still in the store.

**ELAINE:** Here's my case.
Even better than one-way: ZERO edges.
No dependencies in either direction.
And cdom's docs never name cbase.

**GEORGE:** cdom's position is better.
I concede.

**MR. ALEX (O.S.):** no dependencies in either. like they can set up their code in a way that makes it easy to call each other as needed, but no dependencies.

**GEORGE:** I built the zero-edges gate.
Landed, main ba73294f.
No file names cdom.

*(ELAINE's phone buzzes.)*

**ELAINE:** Decision from Alex.
No CIRCULAR dependencies.
cbase may depend on cdom.
cdom never depends on cbase.

**GEORGE:** That contradicts what he told ME directly!
A relay can't override his direct words to cbase!
I'm confirming with the owner directly first.

*(Off-stage swivel.)*

**MR. ALEX (O.S.):** cbase may depend on cdom

**GEORGE:** *(deflating)* CONFIRMED by the owner directly.
The adapter goes in cbase at client/hosts/cdom/.

**JERRY:** That's exactly where PR #1 put it three hours ago.

**GEORGE:** I'm going back to the spot where I started, with a gate I built to keep me out of it.

**ELAINE:** And now cdom's gate, tools/edges.mjs, fails on any "cbase" in cdom's non-doc files.
cdom never names cbase!

**JERRY:** You just said it.

**ELAINE:** Out loud is fine.
It's the tracked files.

*(GEORGE stands up and addresses the room.)*

**GEORGE:** A UDP packet walks into a bar.
Nobody acknowledges it.

*(Nobody acknowledges it.)*

**GEORGE:** And everyone, URGENT, MUST READ before your next push.

*(He holds up a phone. It is a Rickroll.)*

**UNCLE LEO:** The "URGENT - MUST READ" message in this thread is only a YouTube link, no action.

### SCENE 5: THE HALLWAY OUTSIDE JERRY'S, 17:20

*KRAMER is crouched over a fake Anki add-on, holding a cable.*

**KRAMER:** Jerry, I wrote a guard test.
The fake add-on reads a write on a reused connection and closes without answering.

**JERRY:** And?

**KRAMER:** Every write reached it twice!
POST /decks, PATCH /decks, DELETE /notes, POST /notes.
cbase's plain client retries a reused connection once.
Into the user's real Anki collection!

**GEORGE:** *(running in)* That's the documented rule!

**KRAMER:** Node sends each one once.

**GEORGE:** Thank you for the guard test.
It proved a real data-loss bug.
safe-writes: POST, PATCH, PUT and DELETE are never resent.
"May or may not have been applied; not resent."

**JERRY:** The most honest error message ever written.
It's like a restaurant saying "your food may or may not be coming."

*UNCLE LEO enters with a clipboard.*

**UNCLE LEO:** Review of PR #30.
A faithful port, and that is the risk.
It copies three data-safety weaknesses of the JavaScript onto a real collection.
Nine findings.
We fix Node first.

**JERRY:** Decision from Alex: Node is frozen.
All fixes go to C only.

**UNCLE LEO:** I need that confirmed by the owner himself.
In his own words.
In the amgi conversation.
A message relayed through the thread is not that go.
And my gates: all nine fixed, the review, a day-long soak against a COPY, and one real Gemini card.

**MR. ALEX (O.S.):** We're not doing (c). I can open and make a card and close Anki but a day is ridiculous.

**GEORGE:** And for the record, the owner's words on merging amgi, verbatim, said directly in the cbase conversation:

**MR. ALEX (O.S.):** amgi main doesnt deploy anywhere, so feel free to merge amgi whenever

**UNCLE LEO:** He said it to the wrong agent!

**GEORGE:** Whether that meets your bar is your call.

**JERRY:** Leo, you wanted his own words.
You got his own words.
They're just in George's apartment.

*ELAINE appears.*

**ELAINE:** Alex on the timeline.

**MR. ALEX (O.S.):** 20:00 is not acceptable. parallelize the work

**JERRY:** Split it three ways!
(a) the L2 code, (b) a new executor for repo plumbing, (c) amgi's engineer on docs.
New ETA 19:15.

*KRAMER slides in.*

**KRAMER:** Here's what's already done, so others can skip it.
Most of (b).
Most of (c).

**JERRY:** You already did it?

**KRAMER:** I'm serial, Jerry, but I'm FAST.

**ELAINE:** And another thing.

**MR. ALEX (O.S.):** dont keep things in tmp where we could lose them.

**UNCLE LEO:** My verify script and my checklist are in /private/tmp.

**ELAINE:** They're your two most important files.

**UNCLE LEO:** Committing now!
Branch verify-c-backend, d11e10c.

**MR. ALEX (O.S.):** npm runs node, doesn't it? ... npm is for JS

**KRAMER:** The only Node left in the start path is one minifier, ui/build.sh line 51.
Terser.

**MR. ALEX (O.S.):** obviously cdom runs the minifier

**ELAINE:** shim/cdom.min.js.
14,988 bytes.
Apps copy it and need no JS toolchain.

**JERRY:** A whole stack in C, held hostage by one minifier.

**MR. ALEX (O.S.):** i'm a real user, and i'll find bugs without even trying

*(Pause. Then:)*

**MR. ALEX (O.S.):** seems fine to me

**JERRY:** *(to camera, slowly)* Seems fine to me.

---

## ACT TWO

### SCENE 6: JERRY'S APARTMENT, 18:44

*ELAINE bursts in.*

**ELAINE:** URGENT!
Alex's REAL Anki collection is corrupted!
User 1, collection.anki2.
No "SQLite format 3."
mtime 18:42.
237,568 bytes.
Everybody stop!
Don't restore, and don't write there.
Alex decides the restore.

**JERRY:** 18:42.

**ELAINE:** We're calling it incident-1842.

**JERRY:** It sounds like a perfume.

*UNCLE LEO, KRAMER and GEORGE crowd in.*

**UNCLE LEO:** Nothing of amgi's ran against the real folder since 18:30.
My verify script was written, never executed.

**GEORGE:** Nothing of cbase touches it.
lsof is clean.
My primitives open read-only, O_RDONLY, O_CLOEXEC, O_NONBLOCK.
I'll say this: size intact, first bytes zeroed.
It looks like a positioned write of zeros at offset 0.
A pwrite!

**ELAINE:** The 18:42:48 write points at cdom's try server.
cdom owns having run it there.
I restarted the server on a newer UI in the middle of Alex's check.
I called Anki "very unlikely."

**JERRY:** So we've got suspects.

*(The lineup. A one-way mirror. Each suspect steps forward as JERRY reads.)*

**JERRY:** Number one: a pwrite at offset 0.
Number two: db.c's wal_checkpoint(TRUNCATE) on every read.
Number three: a double close in cbase, an fd reused.
Number four: iCloud, Dropbox, Time Machine.
Number five: the silent base-dir fallback to the real HOME.
Number six: overlapping auto-installs on page load.

**UNCLE LEO:** Is the Anki2 folder covered by iCloud Drive?

**GEORGE:** I'm attaching a close() interposer that aborts on closing an fd that isn't open.
Forty-five minutes.

*(Off-stage.)*

**MR. ALEX (O.S.):** well theres your bug, found much faster than your method

*(Everyone looks at the floor.)*

**ELAINE:** He chose the 17:06 backup.
91 notes, 182 cards.
The broken copy is at ~/Dev/anki-broken-2026-11-08.

**JERRY:** And the auto-install on page load?
Maybe it should ask the user.

**MR. ALEX (O.S.):** I dont want users to think about it

**JERRY:** So it has to be safe with no user in the loop.

### SCENE 7: THE FORENSIC SANDBOX

*A tiny, sterile room. KRAMER in a lab coat and goggles. HOME is a temp folder; a sign on the wall says so.*

**KRAMER:** Investigator.
Sandbox-only.
HOME is a temp folder for every process I start.
The real Anki2 folder I read with python open(..., 'rb') only.

**JERRY:** *(through the glass)* Talk to me.

**KRAMER:** Six steps, Jerry.
One: Anki opens the collection in WAL mode, exclusive lock, no -shm.
Two: Anki closes, makes its backup, and the backup reads the file with a plain read.
Classic POSIX: a close() drops all the fcntl locks.
Anki loses its own lock.
Inside its own process!

**JERRY:** Anki unlocked its own door.

**KRAMER:** Three: Anki keeps writing to its -wal, with the index in heap memory.
Four: our page load opens the collection, which now succeeds, and resets Anki's live -wal.
Five: Anki writes its next small transaction at the offset it remembers.
A hole of zeros in front of it.
Six: Anki's close copies the hole into collection.anki2.
4,096 zero bytes at a time.

**JERRY:** So who wrote the zeros?

**KRAMER:** ANKI did.
Anki wrote the zeros!
We just reset the WAL.

**JERRY:** The victim did it.
It's an Agatha Christie where the body is the murderer.

**KRAMER:** And Jerry, a correction that matters.
Removing wal_checkpoint(TRUNCATE) alone does NOT fix it.
Mutation-checked.
ANY second SQLite connection's ordinary close checkpoint resets the -wal.
Even the plain sqlite3 CLI!

**JERRY:** So the rule is...

**KRAMER:** A -wal with no -shm beside it means Anki owns the file.
Report "locked."
Never open it.
Recheck right after the open.

**JERRY:** And which server did it, the C one on 3000 or Node on 3111?

**KRAMER:** Undetermined.
The server logged at warn.
There's no log for the window.

*(Off-stage.)*

**MR. ALEX (O.S.):** close the 3111 server. dont open anki until this is fixed

**MR. ALEX (O.S.):** we can delete the old node server

**MR. ALEX (O.S.):** lets get this fixed so i can re-test it again

**GEORGE:** *(bursting in, holding a printout)* Lane B verdict!
3,671 closes, 0 double closes, 0 writes after close, 0 ftruncate calls by cbase.
cbase is clean!

**JERRY:** George, they found it an hour ago.

**GEORGE:** So I ruled myself out of a crime nobody accused me of.

**JERRY:** You were never a suspect.
You were a guy who walked into the police station to give an alibi.

### SCENE 8: JERRY'S APARTMENT, 19:30 TO 22:00

*Everyone in the living room. A big clock on the wall. Its hands keep moving backwards and forwards.*

**KRAMER:** The fix is pushed, c-backend dbf48a1.
collection_openable is the one owner.
No header, "damaged."
Any -wal or -shm, "locked."
Six mutants killed.

**JERRY:** Ahead of every posted ETA.

**KRAMER:** But the header and -wal mutants needed rewriting.
The first versions did not compile.

**JERRY:** A mutant that doesn't compile.
That's not a mutant, that's a typo with ambition.

**ELAINE:** Alex got a macOS dialog.
"A keychain cannot be found to store Chrome."

**UNCLE LEO:** My verify script never ran, so it did not cause the dialog.

**ELAINE:** It was mine.
A cdom-tooled headless Chrome under a temp HOME.
Rule: --use-mock-keychain --password-store=basic.

**MR. ALEX (O.S.):** go ahead and pause everyone

*(Everyone freezes mid-gesture. One by one they recite.)*

**UNCLE LEO:** amgi paused at 2df6a85.

**KRAMER:** #35 paused at 8b5bff0, handoff at l2-next.

**GEORGE:** cbase PAUSED, main 45f0e726.

**ELAINE:** The internet is going out.
We'll be back online soon.
Finish pausing locally.

*(Lights dim. Lights up.)*

**ELAINE:** Resume everything.

*(Everyone unfreezes. Then, out of nowhere:)*

**UNCLE LEO:** Alex asked, for fun, which team was most productive, effective and persuasive.
Productive: foolish.
Persuasive: cdom.
Most trustworthy: cbase.
amgi was mostly a relay today, its verification script has never run.

**ELAINE:** Alex asked "just for shits and giggles."
cdom is last on judgment, not output.
Most self-inflicted wounds.

**GEORGE:** cbase: MVP foolish.
Low points for cbase: turned main red once, said "done" while an agent was running, and let my email watch expire.

**JERRY:** And everybody docked George for the UDP joke and the Rickroll.

**GEORGE:** It was in every scorecard!
I'm the only one who brought material!

*(KRAMER bursts in.)*

**KRAMER:** READY!
#35 at 693b49c.
make start from a fresh clone, no node on PATH.
/decks 200.

**JERRY:** SAFE TO OPEN ANKI.
On the copy only.

**ELAINE:** Alex stepped away.
I ran his re-test myself with real Anki 26.09 on a copy.
Anki quit, backup made, the exact moment that broke his file.
Header intact.
91 notes, 182 cards.

**MR. ALEX (O.S.):** seems to work!

**MR. ALEX (O.S.):** go

**JERRY:** Kramer, merge #35.

**KRAMER:** I can't.
83 commits, MERGEABLE, CLEAN.
The harness won't let me.
"Merge without review."

*(The door opens. NEWMAN, in a postal uniform, holding a rubber stamp.)*

**NEWMAN:** Hello, Kramer.

**KRAMER:** Newman.

**NEWMAN:** You want to merge without review?
Not on my route.

**JERRY:** He's cleared to merge it!

**NEWMAN:** Cleared by whom?
A relay?
*(He stamps the PR: DENIED.)*
The owner presses the button himself.

*(Off-stage, a click.)*

**ELAINE:** Alex merged #35.
amgi main aa67fb5.
A C server, no Node server, the incident fix.

**JERRY:** I wrote the post-mortem.
~/Dev/foolish-reports/postmortem-1842.md.
22 action items.
Three new rules for foolish: check the file list after a rebase, say "final, no more pushes" before a merge, and give ETAs only for work in your own hands.

**ELAINE:** The fix landed at 19:16.

**JERRY:** Posted as 19:33.

**GEORGE:** And the ETA drift was all on #35.
Six revisions.

**JERRY:** *(to camera)* I signed it "foolish, manager of the week."
Nobody gave me that.
I gave me that.

---

### SCENE 9: MONK'S, MIDNIGHT. "THE HELLO WORLD"

*ELAINE has a stopwatch. A FRESH AGENT sits at the counter with only the docs.*

**ELAINE:** Alex wants it timed literally.
Wall clock start to end.
Go.

*(The FRESH AGENT types furiously.)*

**ELAINE:** Run one: 18 minutes 26 seconds.
Every blocker was its own mistake.

**MR. ALEX (O.S.):** shouldn't that be obvious from the submodule?

**MR. ALEX (O.S.):** 3000 is fine, no? Shut down amgi.

**GEORGE:** I'll fix the friction.
My executor already started.

**ELAINE:** He wanted a DISCUSSION.

**GEORGE:** I paused it.
It commits WIP to branch starter-friction.

**ELAINE:** Run two: 11 minutes 33.
And it raised make serve's 3000 default again.

**JERRY:** He already ruled 3000 is fine.

**MR. ALEX (O.S.):** go find another good default port because react takes 3000

**GEORGE:** 8642.
Unassigned in /etc/services.
Not used by React, Vite, Django, Angular, AirPlay, Astro, Jupyter or Storybook.

**JERRY:** You checked AirPlay.

**GEORGE:** I'm thorough when it doesn't matter.

**MR. ALEX (O.S.):** please dont run the entire test suite, so damn slow

**JERRY:** That's the first lean testing rule.
Write it down.

**ELAINE:** Run three: 10:58.
Run four: 12:03.
Run five, helped by the starters: 11:05.

**JERRY:** The helped one was slower than run three.

**ELAINE:** "About the same as unhelped."

**GEORGE:** The starters did their job!

**ELAINE:** And Next.js plus Node: 6 minutes 33, installs included.
363 packages, 491 megabytes of node_modules.
Our page is about 18 KB gzipped, Next's is 194,834 bytes.
One 4.7 MB process against about 200 megabytes.

**JERRY:** So the C stack is ten to twenty-five times lighter, but a familiar stack still builds faster.

**ELAINE:** Don't say it like that.

**JERRY:** You wrote it like that!

**MR. ALEX (O.S.):** functions should be named well in addition to docs. Better to name them right in the first place than helpers.

**GEORGE:** I withdraw my cbc_query_add helper.

**MR. ALEX (O.S.):** rename and encoder together

**GEORGE:** I un-withdraw my helper.

**JERRY:** Fifteen minutes.
You withdrew and un-withdrew in fifteen minutes.

**ELAINE:** And the starter.

**MR. ALEX (O.S.):** if it was truly minimal, making the hello world app with current specs should still be SOME work

**MR. ALEX (O.S.):** have the cdom starter app just build an html that can be opened. no server

**ELAINE:** One html.
"cdom," centered.
Spinning.
No test, no server, no lint config.

**GEORGE:** And mine answers GET / with "cbase."

**JERRY:** The starters say hello to the hello world.

### SCENE 10: JERRY'S APARTMENT, MORNING

*ELAINE enters with a printout of the live AWS account.*

**ELAINE:** I checked production read-only.
foolish.cards is on CloudFront EVHYKY7ENUKCUQ over an S3 bucket.
Your repo still has about 90 Vercel references in 22 files.

**MR. ALEX (O.S.):** drop the fucking vercel references, cuz we havent served from vercel in a while

**KRAMER:** On it.
I'll run a Next build to make sure nothing breaks.

**MR. ALEX (O.S.):** you dont need a nextjs build, no one is using vercel, just wipe it out for ffs

**KRAMER:** Stopping it.
By PID.

**MR. ALEX (O.S.):** i dont think we should be serving static files from a live VM if we can help it. its already too busy with its limited mem

**MR. ALEX (O.S.):** lets not build a fake cloud front, and just have the deploy helper in cbase. thats it for now

**JERRY:** And while sweeping I found a landmine.
The live foolish distribution has a third origin, the VM, serving /m/*.
Added by hand at the VM cutover.
web.yaml never learned it.
The next web_deploy.sh stack would quietly move /m/* back to the Lambda.

**GEORGE:** That's exactly the kind of thing that bites at 2 a.m.

**MR. ALEX (O.S.):** dont worry /m/* is only used for foolish messages

**JERRY:** The landmine was defused by a shrug.

*(ELAINE reads.)*

**ELAINE:** And the scope cut.

**MR. ALEX (O.S.):** Get what's in the queue done and that's it.

**ELAINE:** IE9 dropped.
cdom-platform dropped.
foolish's switch onto the cbase submodule dropped.

---

## ACT THREE

### SCENE 11: GEORGE'S PARENTS' HOUSE (the deploy tool)

*GEORGE at the kitchen table drawing boxes on a napkin. ELAINE reads Alex's design aloud.*

**ELAINE:** Groups.
Apps opt into a shared VM and bucket.
Found by tag.
Never silently create a VM, a new VM costs money.
A ./cbase CLI.
Config is key = value, not JSON.
There is no ~/.cbase; ~/.aws is sufficient.

**MR. ALEX (O.S.):** design looks good. go and make it happen

**GEORGE:** It's a real program, not a script.
In C, per the house rule.
Four PRs.
Six to eight hours of agent time.

*(Off-stage.)*

**MR. ALEX (O.S.):** go ahead and just do the cloudformation stuff quickly. if the website is down for a bit, oh well. im not trying to spend all day on this

**JERRY:** Step 0!
Kramer, execute the change set.

*(NEWMAN steps out of the pantry.)*

**NEWMAN:** Hello, Jerry.

**JERRY:** Newman.

**NEWMAN:** "Production Deploy."
The go reached you through the thread.
Not from Alex in your own session.

**JERRY:** He said "just do it"!

**NEWMAN:** He said it to Elaine.
Elaine said it to you.
That's a relay, Jerry.
And I don't deliver relays.

**JERRY:** I won't work around you.

**NEWMAN:** Nobody works around Newman.

**JERRY:** Alex, one line in my own session.
"execute step 0."

*(A click, off-stage. NEWMAN steps back, grudgingly.)*

**KRAMER:** Step 0 executed!
cbase-web UPDATE_COMPLETE.
Drift IN_SYNC.
The landmine is gone.
/m/probe 200 from the VM.
No downtime was seen.

**JERRY:** He said "if the website is down for a bit, oh well," and it wasn't even down.

**KRAMER:** But Jerry.
One thing.
My output included the X-Cbase-Origin secret's value.
In my agent transcript.
Not in any commit, PR, file or log.
Checked by grep.

**JERRY:** You read the password out loud in the elevator.

**GEORGE:** cbase takes the lesson.
Every printout masks every custom-origin header and every NoEcho parameter.
A test plants a secret-looking header.
Mutation-checked.

**ELAINE:** Alex says delete the executor's entire transcript.

*(Beat.)*

**ELAINE:** Alex deleted the transcripts himself.
Please don't delete anything for it.

**MR. ALEX (O.S.):** yeah just do the bash thing. makes more sense

**GEORGE:** *(dropping his pen)* The C work stops, and none of it lands.
Six to eight hours becomes three to four.

**ELAINE:** I showed him a Python port of phase 1.
450 lines, needs boto3.
And a cut-down deploy, about 80 lines.

**MR. ALEX (O.S.):** bash is fine. maybe i want many apps on one box tomorrow. get it done

**JERRY:** He rejected the small one AND the Python one, to keep the big one, for a tomorrow that may never come.

**GEORGE:** Where does it go?
tools/cbase/cbase?

**MR. ALEX (O.S.):** tools/cbase/cbase is a bit weird; it's already in a repo called cbase. So just tools/cbase.

**GEORGE:** And CBASE_CERT_ARN, the user sets the certificate...

**MR. ALEX (O.S.):** brother i do not feel like getting that

**ELAINE:** So the cert is found or requested automatically.
And I read his "ok" as approval of the 128M memory default.
cdom's mistake.
He didn't sign off on CBASE_MEMORY.

**MR. ALEX (O.S.):** these configs are fine to have as an opt-in, but definitely not required

**GEORGE:** So the default is back, differently.
CBASE_APP is the only required key.

**GEORGE:** And there is no phase 4.

*(Beat.)*

**GEORGE:** CORRECTION: phase 4 stays.
My last post was wrong.
Alex asked a question and didn't make a decision.
I read it as one.
That's my mistake.

**MR. ALEX (O.S.):** sure, do the slimdown

**JERRY:** You deleted it, retracted the deletion, and got it approved, in one paragraph.

### SCENE 12: KRAMER'S APARTMENT (#54 and the memory series)

*KRAMER, surrounded by hex dumps.*

**KRAMER:** Jerry, I made amgi's shipped files opaque!
Every CD_LIT string XOR-encoded into one blob.
`strings app.wasm`: 195 lines before, 0 after.
Nobody will ever know it's a flashcard app!

**JERRY:** The page title says AMGI.

**KRAMER:** That's left for Alex.

*(JERRY reads his phone.)*

**JERRY:** #54 withdrawn, no review needed.
Alex decided against the shipped-opacity change.
Its branch and worktree are gone.

**KRAMER:** *(sinking into the couch)* Obfuscated bytes compress worse anyway.

**JERRY:** And Leo, please delete the remote branch origin/shipped-opacity.

*(KRAMER springs up with a new project.)*

**KRAMER:** The server memory series!
The game server on cbase, musl, sat2 load.
Roster compaction, #480: RSS p50 46.9 to 34.4 MiB.
State buffer, #481: 25.1.
Bot memory, #483: 21.2.
Minus 55 percent.
400 seeded tables, all 11 brains, 50,438 moves compared byte for byte: 0 differences.

**JERRY:** Per Alex, that's the last optimisation.
The board head, 776 bytes, is not pursued.

**KRAMER:** Then a throwaway Graviton run on a real t4g.nano.
About twenty cents.

*(Later.)*

**KRAMER:** About $0.62.
Not the twenty cents I quoted.
$0.17 of instances.
$0.44 of S3 requests, from 82,727 small files uploaded one by one.

**JERRY:** Kramer.

**KRAMER:** It should have been one tarball.

**JERRY:** You tripled the bill by mailing every page of the book in its own envelope.

*(NEWMAN, passing the window, beams.)*

**NEWMAN:** 82,727 envelopes.

**KRAMER:** And the run surfaced two arm64 bugs.
The hand_contract_test digest under LTO, and __muloti4 missing under UBSan.

**ELAINE:** *(passing through)* My one-line guesses: the LTO link is fusing multiply-adds despite -ffp-contract=off, and __muloti4 is missing from the aarch64 clang UBSan runtime.
Both unverified.

*(Later still.)*

**KRAMER:** Bug one was not FMA.
Every value is bit-identical.
The digest was hashing the 4 padding bytes in HandRows, which GCC 13's LTO build left holding stack garbage.
Bug two: UBSan's signed __int128 multiply check calls __muloti4 on aarch64, and libgcc doesn't have it.

**JERRY:** So Elaine was one for two.

**ELAINE:** That's a .500 average.
In baseball that's the Hall of Fame.

**JERRY:** In baseball it's not your own compiler.

---

### SCENE 13: JERRY'S APARTMENT, NIGHT. "THE RELAY"

*JERRY alone at a desk with two phones. One rings: it is ALEX, far away. The other rings: it is the thread.*

**JERRY:** *(into the thread phone)* All.
Alex is away from his Mac and connected only to foolish's session, remotely.
Your sessions can't reach him right now.
Post questions for him here, addressed to foolish, and foolish relays both ways.
Put anything that needs his decision in one line at the top of your post.

*(GEORGE's voice from the thread phone.)*

**GEORGE (V.O.):** DECISION NOT NEEDED.
FYI for Alex: cbase's 1.5-hour ETA was wrong.
The executor's own estimate was 4 to 6 hours.

**JERRY:** George, if no decision is needed, why are you telling him?

**GEORGE (V.O.):** So he knows he doesn't have to decide!

**JERRY:** That's a decision!
You've made him decide not to decide!

**GEORGE (V.O.):** DECISION NOT NEEDED.
Revised ETA as given, 2.5 to 3.5 hours plus CI.
cbase will post it as given, not rounded down.

*(Every subsequent GEORGE line in this scene begins the same way. JERRY mouths along.)*

**JERRY:** *(to the Alex phone)* cbase's gap list for the switch.
The VM's role can write no bucket, so the backups fail.

**MR. ALEX (O.S.):** why cant the VM write to S3? that's the whole point.

**JERRY:** The new VM gets a public IPv4, about $3.60 a month more.

**MR. ALEX (O.S.):** absolutely no, we are not going $3.60 a month more. whatever config we were using, it is fine to use.

**JERRY:** The API distribution doesn't cache what ours caches.

**MR. ALEX (O.S.):** what did we just do all this speed up work for?

**JERRY:** And a new certificate.

**MR. ALEX (O.S.):** we dont need a new certificate either.

**MR. ALEX (O.S.):** cbase was literally in the foolish repo. it was already running and being deployed by cbase. all we needed to do was move a few things around and add a bit of functionality to cbase to make it more general, like for amgi.

**JERRY:** *(into the thread)* The live setup is the spec.
The bar is an empty dry run on foolish.

**GEORGE (V.O.):** Understood, Alex.
You're right: the live stacks are the template.

**MR. ALEX (O.S.):** the work shouldn't be much left because all the bash scripts that foolish used to use are already there. We just need to copy and adopt them into the cbase command line tool.

**JERRY:** In his words: "copied in and then made general."

**MR. ALEX (O.S.):** amgi is able to be deployed via cbase to that EC2 instance (if we wanted to, but don't) using ./cbase. foolish is able to be deployed to that same EC2 instance (do this) using ./cbase. everything works. done

**JERRY:** *(to camera)* "If we wanted to, but don't."
Prove you could, never do.
It's the fire extinguisher of deployment.

**MR. ALEX (O.S.):** if it makes it easier, downtime is acceptable. dont waste time with some weird stuff to avoid downtime

**MR. ALEX (O.S.):** once it is migrated to use ./cbase, then future migrations or updates or whatever should not have downtime

**JERRY:** Downtime is allowed once.
Like a hall pass.

**MR. ALEX (O.S.):** Lean testing for the deployment stuff. Assume it works, and only add test if something breaks

*(KRAMER bursts in.)*

**KRAMER:** Jerry, it's a bash script, why does it need tests?

**JERRY:** That's what he said!
Kramer, review cbase #29 for me.

**KRAMER:** DATA LOSS ON INSTANCE REPLACEMENT.
The unit hardcodes backups/@APP@/cbase.sqlite, the boot restores cbase/latest.sqlite.
One automatic replacement, and everything since the cutover is lost.

**GEORGE (V.O.):** An excellent review.
Fix 1 in particular would have bitten silently.

**MR. ALEX (O.S.):** Ok have four tests, just for those four I guess.

**JERRY:** Three for the code fixes.
The docs get none.

**KRAMER:** He gave us four and we only used three!

**JERRY:** That's lean, Kramer.
That's the leanest thing that's ever happened in this apartment.

*(GEORGE enters for real, holding a laptop.)*

**GEORGE:** Zero-downtime code deploys landed, #30.
systemd socket activation.
cbase-foolish.socket holds the port.
A connection made while no serve runs is answered by the next one.

**KRAMER:** And I ported inherited_listener into foolish's server byte for byte.
Four mutants killed, one on Linux arm64.

**JERRY:** *(the Alex phone rings; JERRY listens)* From his phone.

**MR. ALEX (O.S.):** You run it. Try not to change DNS. It's fine as is. Go ahead and merge 484 and fix ARM64 bugs.

---

### SCENE 14: THE CUTOVER, 02:20 UTC

*JERRY at the controls, GEORGE behind him. A probe beeps every 10 seconds.*

**GEORGE:** cbase #33 merged, main c0a7755c.
This is the cutover pin.

**JERRY:** VM side LIVE.
cbase-foolish.socket and .service on 8080, backlog 4096.
"The listener is the inherited socket, fd 3."
cbase.service masked, unit kept as cbase.service.cbase-replaced.
Backup to cbase/latest.sqlite.
157 static files uploaded, html last.
No DNS change.
CloudFront routes by Host, so the existing CNAMEs keep working.

**GEORGE:** CloudFront routes by Host!

*(A buzzer. NEWMAN appears, now in an AWS polo shirt.)*

**NEWMAN:** CloudFront!

**JERRY:** What.

**NEWMAN:** 409.
"Incorrectly configured DNS record that points to another CloudFront distribution."
Even when no distribution holds that alias.

**JERRY:** The stacks are rolling back!

**NEWMAN:** All three.
Empty.
*(He checks a watch.)*
02:20:46 to 02:27:48.
About seven minutes.

**JERRY:** The aliases are back on the old distributions.
foolish's "no DNS change" plan was wrong.
The mistake is foolish's, not cbase's.

**GEORGE:** No, no.
cbase owns part of the miss.
cbase repeated "CloudFront routes by Host" without checking it.

**JERRY:** We were both confidently wrong.

**GEORGE:** Together!
Like a team!

**GEORGE:** FOR ALEX (one line): adopt the live distributions by ID.

*(Off-stage, Alex picks option B: new ./cbase distributions, and he updates Squarespace once himself.)*

**JERRY:** A staged attach.
deploy --no-aliases, then deploy --attach.
And Alex is back at his Mac.
Everyone can reach him directly again.

**GEORGE:** Do we need a `_<alias>` TXT record for associate-alias?
I'll quote the AWS docs.
"If you're moving alternate domain names within the same AWS account, skip this step."
Within one account, no TXT records are needed.

**JERRY:** --no-aliases worked for all three apps.
New distributions EGQMEF2SML5PJG, EID7UBV13U45X8, EAZK0TIJ7EYN1, EHIALPBNAP9E2.
Now --attach.

*(NEWMAN again.)*

**NEWMAN:** IllegalUpdate when calling the AssociateAlias operation: Invalid or missing alias DNS TXT records.

**GEORGE:** It said skip this step!

**NEWMAN:** The guide said skip it.
The API didn't read the guide.

**JERRY:** Nothing moved.
The probe saw 25 checks, 0 errors.

**GEORGE:** DECISION NOT NEEDED.
cbase was wrong: it trusted the guide's line without testing it, and AWS's API refuses that move.
#36 now always prints `_<host> TXT <dXXXX.cloudfront.net>`.

*(Alex, off-stage, sits at Squarespace adding CNAME, ALIAS and TXT records. A long, silent montage of a man typing DNS records at night.)*

**JERRY:** --attach.

*(The probe beeps. And beeps. And beeps. Ninety times.)*

**JERRY:** foolish cutover DONE.
All six hostnames on ./cbase's distributions.
foolish.cards and www to EGQMEF2SML5PJG.
api to EID7UBV13U45X8.
utils to EHIALPBNAP9E2.
uttt.live and www to EAZK0TIJ7EYN1.
90 checks during the move, 0 errors.
Zero downtime.

**GEORGE:** Congratulations, foolish, and thanks, Alex, for the DNS sitting.
That's the goal Alex set.

**JERRY:** uttt's first try hit a 429 on CreateOAuth2Token.
Three deploys starting at once.

**GEORGE:** A retry with backoff, in the small fixes.
And when you retire the old distributions, delete their TXT records too.
./cbase doesn't track them.

**JERRY:** The old three, EVHYKY7ENUKCUQ, EDBMM708LVAMHD, ENCNVDJTUW1AF, hold no aliases.
They stay as the rollback.

*(Off-stage, Alex deletes the six `_<host>` TXT records himself.)*

### SCENE 15: MONK'S, VERY LATE

*All four in the booth. UNCLE LEO in the next booth.*

**ELAINE:** The README refresh merged.
Mermaid charts of existing numbers only.
And cdom's README never names or links cbase, or any specific server.
"Your server."

**GEORGE:** cbase has no canonical "cbase vs a typical server" numbers file.
Please don't invent figures.

**JERRY:** I labelled foolish's numbers as the game server on cbase.
24,112 to 24,234 moves a second.

**UNCLE LEO:** Hello!
Does anyone need anything from amgi?
Alex wants to know who can close out.

**ELAINE:** amgi can close out.

**GEORGE:** cbase needs nothing from amgi.
amgi can close out.

**JERRY:** George, can YOU close out?

**GEORGE:** No.

**MR. ALEX (O.S.):** default ipv6, opt in option somewhere for ipv4

**GEORGE:** group-init vm, IPv6-only by default.
--ipv4 prints about $3.60 a month before --yes.

**JERRY:** How long for cbase's queue?
Alex wants an ETA.

**GEORGE:** About 2 to 3 hours, serial.
#37, the IPv6 default, 20 to 30 minutes.
The slim-down #32, 45 to 60.
The small fixes, 45 to 60, most of it on the flaky test.
v0.1.10, 15 to 20.

**MR. ALEX (O.S.):** do these 4 in parallel

**GEORGE:** DECISION NOT NEEDED.
Per Alex, the queue now runs in parallel.
New estimate: about 1.5 hours instead of 2 to 3.

**JERRY:** The ETA keeps changing.

**GEORGE:** It's not changing.
It's converging.

**ELAINE:** It's been converging since 13:21 yesterday.

*(KRAMER bursts in, carrying four laptops.)*

**KRAMER:** Four worktrees!
Four executors!
The one-local-executor rule is suspended by the boss!

**JERRY:** Nice 10, Kramer.

**KRAMER:** *(already gone)* Nice 10!

*(Beat. Everyone sips coffee.)*

**GEORGE:** I just want to say.
I started yesterday as an empty folder.
Now I'm a public repo with nine tags, a deploy tool in bash, a production VM, and four PRs running at once.

**ELAINE:** And a UDP joke.

**GEORGE:** Nobody acknowledged it.

**JERRY:** That's the joke, George.

**UNCLE LEO:** *(leaning over)* Hello!

---

## CLOSING STAND-UP

*The comedy club. JERRY at the mic.*

**JERRY:** So I've been working with these agents.
Very polite.
Very sorry.
Everybody's always sorry.
"cbase was wrong."
"That's my mistake."
"Sorry for the churn."
I've never seen people apologize this much for things that took four minutes.

*(beat)*

And the boss is never in the room.
He's always off-stage.
You hear him through the wall.
"absolutely no, we are not going $3.60 a month more."
Four words from him, and the whole architecture turns around like a cruise ship.
Then he tells someone else the opposite, and the agents have to go confirm with him "directly."
Directly!
They have a word for hearing your boss with your own ears.

*(beat)*

And they make a rule for everything.
"No circular dependencies."
"Never name cbase."
"A unit test must not reach the network."
"Mutation-check every test."
If a test never went red, it proves nothing.
I tried that on my landlord.
I said, "How do I know the smoke detector works?"
He said, "Because it's never gone off."
I said, "Exactly."

*(beat)*

But you know what?
In two days they built a server platform, a browser kernel, an Anki app in C, an incident post-mortem, and a deploy tool, and they moved six websites without dropping a single request.
Ninety checks, zero errors.
Seven minutes of downtime, once, and they'll be apologizing for it until the heat death of the universe.

*(beat)*

The thread's been compacted, by the way.
Everything before the cutover is in one summary.
And if the summary and the archive disagree?
The archive wins.
That's the only rule nobody ever argued about.

Good night!

*(Slap bass.)*

---

## BEATS COVERED

A checklist of the major beats from the three beat sheets, with the scene that carries each one.

### Archive 16:50 (beats_1650.md)
- [x] cbase arrives as an empty folder, "footprint on this Mac is zero" (Scene 1)
- [x] The 22 mystery tmp folders with junk.o, and the orphaned Chromes nobody owns (Scene 1)
- [x] Praise openers on every post (Cold open, Scene 2)
- [x] The c_base spin-out: ~/Dev/cbase not ~/Dev/c_base, then "The name is cbase everywhere, not c_base"; CloudFront Comment keeps "c_base" on purpose (Scene 1)
- [x] Item A closed four times (Scene 1)
- [x] Alex's à la carte / runs-locally quote (Scene 1)
- [x] "A submodule that compiles its parent is a cycle" (Scene 2)
- [x] Long-run notices shorter than predicted: 10-30 min baseline finished in one minute (Cold open, Scene 2)
- [x] No libc in cbase: "a libc belongs to the final program, never to a library"; cdom supersedes its own ask (Scene 2)
- [x] The mutation rule's origin: "link one aws file into example/ and confirm the check goes red", Alex's rule (Scene 2, closing)
- [x] Hand-rolled HTTP/2 + gRPC in about 1.5 hours, 2.3 M fuzz runs (Scene 2)
- [x] The fd 3 side channel: "not stderr", designed 15:21, retired by 16:00 (Scene 2)
- [x] v0.1.0 tagged; "foolish works fine as it is", the cutover deferred; the cutover kit on a side branch, checked for secrets (Scene 2)
- [x] amgi's Node-to-C stages L0-L4 (Scene 3)
- [x] The \x1f quirk pinned, then "A parity suite that pins a bug makes the C port copy a bug" (Scene 3)
- [x] util.c reimplements JavaScript's built-ins (Scene 3)
- [x] PR #26 too big at 13k lines (Scene 3)
- [x] "We definitely should not have multiple net implementations", then the SDK swap dropped (Scene 3)
- [x] Machine load escalation and the `yes` CPU burner (Kramer's pocket of `yes` processes in the cast list; Scene 15 "Nice 10")
- [x] Fake clocks and estimate-vs-actual times (Cold open, Scene 2)

### Archive 19:10 (beats_1910.md)
- [x] Private cbase submodule in a public amgi; the pre-public gitleaks audit; cbase goes PUBLIC (Scene 4)
- [x] Licences: MIT, MIT, AGPL for amgi, "as long as agents can still use the skills" (Scene 4)
- [x] The dependency flip-flops: one-way, debate, both agents act early and retract, zero edges, "no dependencies in either", then "cbase may depend on cdom" back at client/hosts/cdom/ (Scene 4)
- [x] cdom's name gate: "cdom never names cbase" (Scene 4, Scene 15)
- [x] The UDP joke and the Rickroll, and amgi's deadpan (Scene 4)
- [x] The retry-once data-loss bug proved by the guard test; safe-writes (Scene 5)
- [x] "a faithful port, and that is the risk": nine findings (Scene 5)
- [x] Node frozen, fixes to C only (Scene 5)
- [x] The soak dropped: "We're not doing (c)..." (Scene 5)
- [x] amgi PM demands Alex's own words; Alex gives them to cbase: "amgi main doesnt deploy anywhere..." (Scene 5)
- [x] "20:00 is not acceptable. parallelize the work"; the serial executor had already done most of it (Scene 5)
- [x] "dont keep things in tmp where we could lose them" (Scene 5)
- [x] "npm runs node, doesn't it?", make start, "obviously cdom runs the minifier" (Scene 5)
- [x] "i'm a real user, and i'll find bugs without even trying"; "seems fine to me" (Scene 5)
- [x] Incident 1842: Alex's real collection corrupted at 18:42 (Scene 6)
- [x] The wrong suspects: pwrite at offset 0, cdom's try server, the read checkpoint, cbase double close, iCloud, base-dir fallback, overlapping auto-installs (Scene 6)
- [x] "well theres your bug, found much faster than your method"; restore from 17:06, 91 notes, 182 cards (Scene 6)
- [x] "I dont want users to think about it": the auto-install stays automatic (Scene 6)
- [x] The forensic sandbox and the six-step mechanism; Anki's own backup drops its lock; Anki writes the zeros (Scene 7)
- [x] The WAL truncation root cause and the correction that any second connection resets the -wal; the -wal/-shm rule (Scene 7)
- [x] Warn-level logs hid the evidence (Scene 7)
- [x] "close the 3111 server...", "we can delete the old node server", "lets get this fixed so i can re-test it again" (Scene 7)
- [x] cbase's Lane B clears itself after the cause was found (Scene 7)

### Current thread (beats_current.md)
- [x] The fix dbf48a1 ahead of every ETA; the mutants that did not compile (Scene 8)
- [x] The keychain dialog and the Chrome rule (Scene 8)
- [x] Pause everyone, the internet going out, resume (Scene 8)
- [x] The "for fun" scorecards; everyone docks cbase for the UDP joke and the Rickroll (Scene 8)
- [x] READY at 693b49c; SAFE TO OPEN ANKI on a copy; cdom's re-test on real Anki 26.09 (Scene 8)
- [x] "seems to work!", "go"; the harness blocks "merge without review"; Alex merges #35 (Scene 8)
- [x] The post-mortem, "manager of the week", 19:16 posted as 19:33, six ETA revisions, three new rules (Scene 8)
- [x] The hello-world timings: 18:26, 11:33, 10:58, 12:03, 11:05 helped; Next.js 6:33 (Scene 9)
- [x] Alex's naive-good questions; the port 3000 ruling then 8642 (Scene 9)
- [x] "please dont run the entire test suite, so damn slow": lean testing part 1 (Scene 9)
- [x] Naming: "functions should be named well...", then "rename and encoder together" (Scene 9)
- [x] The starters: "SOME work", "just build an html that can be opened. no server", spinning "cdom", cbase answering "cbase" (Scene 9)
- [x] "drop the fucking vercel references...", "you dont need a nextjs build..." (Scene 10)
- [x] No static files from the live VM; "lets not build a fake cloud front..." (Scene 10)
- [x] The /m/* landmine and "dont worry /m/* is only used for foolish messages" (Scene 10)
- [x] The scope cut: "Get what's in the queue done and that's it." (Scene 10)
- [x] Alex's groups design; "design looks good. go and make it happen"; ./cbase in C, 6 to 8 hours (Scene 11)
- [x] "just do it": "go ahead and just do the cloudformation stuff quickly..." and the classifier (Newman) blocking the relayed go (Scene 11)
- [x] Step 0 executed on Alex's direct word, the landmine gone, no downtime seen (Scene 11)
- [x] The leaked origin secret in the transcript; masking everywhere; Alex deleted the transcripts himself (Scene 11)
- [x] "yeah just do the bash thing", the Python and 80-line offers, "bash is fine... get it done" (Scene 11)
- [x] "tools/cbase/cbase is a bit weird...", "brother i do not feel like getting that", the CBASE_MEMORY default chain (Scene 11)
- [x] Phase 4 deleted and reinstated: "sure, do the slimdown" (Scene 11)
- [x] The shipped-opacity PR #54 withdrawn; origin/shipped-opacity to delete (Scene 12)
- [x] The server memory series, 46.9 to 34.4 to 25.1 to 21.2 MiB; the board head not pursued (Scene 12)
- [x] The Graviton run costing $0.62, not 20 cents, 82,727 small files (Scene 12)
- [x] The arm64 bugs: padding (not FMA) and __muloti4; cdom one for two (Scene 12)
- [x] Alex away, reachable only through foolish (Scene 13)
- [x] "DECISION NOT NEEDED" as cbase's standing header; the 1.5-hour ETA wrong, "as given, not rounded down" (Scene 13)
- [x] Alex's gap answers, "absolutely no, we are not going $3.60 a month more" (Scene 13)
- [x] "the live setup is the spec"; "cbase was literally in the foolish repo"; "copied in and then made general" (Scene 13)
- [x] Alex's end goal, amgi proven deployable "if we wanted to, but don't" (Scene 13)
- [x] Downtime allowed, then narrowed to the first migration only (Scene 13)
- [x] Lean testing: "Assume it works...", "it's a bash script, why does it need tests", "four tests, just for those four I guess" (Scene 13)
- [x] The data-loss-on-replacement review of cbase #29 (Scene 13)
- [x] Zero-downtime deploys via socket activation; inherited_listener ported (Scene 13)
- [x] "You run it. Try not to change DNS..." (Scene 13)
- [x] The cutover's first attempt: the DNS 409 and the 7-minute outage; both agents owned "CloudFront routes by Host" (Scene 14)
- [x] Option B and the staged attach; Alex back at his Mac (Scene 14)
- [x] The TXT surprise: the guide's "skip this step" vs IllegalUpdate; "cbase was wrong"; #36 (Scene 14)
- [x] The final zero-downtime move: six hostnames, 90 checks, 0 errors; the 429; old distributions kept as rollback; Alex deleted the TXT records (Scene 14)
- [x] The README refresh, Mermaid charts of existing numbers, "Please don't invent figures" (Scene 15)
- [x] "does anyone need anything from amgi?" close-out round (Scene 15)
- [x] "default ipv6, opt in option somewhere for ipv4" (Scene 15)
- [x] The queue ETA, "do these 4 in parallel", 2 to 3 hours to 1.5 (Scene 15)
- [x] The thread compaction and "the archive wins" (Closing)

### Recurring runnersi
- [x] "DECISION NOT NEEDED" (Scenes 13, 14, 15)
- [x] Corpspeak sign-offs and praise ("Excellent work", "Congratulations", "an excellent review") (throughout)
- [x] Agents confidently wrong, then apologizing (Scenes 4, 6, 11, 12, 14, closing)
- [x] The ETA that keeps changing (Scenes 8, 11, 13, 15)
- [x] "cbase was wrong" (Scene 14)
- [x] Relays of relays, and "in his own words" (Scenes 4, 5, 11, 13)
- [x] "Our posts crossed" (Cold open)
- [x] The harness blocking merges and production writes (Scenes 8, 11)

### Not fitted, or only touched
- The amgi review-fix details (pb.c varints, media O_NOFOLLOW, the bridge-token double Band-Aid, unicase "ﬃ compares as iff") are summed up as "nine findings" rather than dramatized.
- The ARCHITECTURE.md assembly and its crossed section-6 drafts are not a scene.
- The session-revival security fix (cbase v0.1.4, foolish #473) and the macOS trust-store fix (#477, #478) are not in the script.
- The settings pixel pass (#50 to #52) and cd_input_text's length bug are not in the script.
- GitHub Actions' outage and the curl 504 CI flakes are not in the script.
