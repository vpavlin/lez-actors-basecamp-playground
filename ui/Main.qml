import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Logos.Theme
import Logos.Controls

// LEZ Actors - pure-QML view over lez_actors_module (Basecamp 0.3). Every call goes through
// callVia() -> logos.callModuleAsync (never a blocking call). Calls and transfers are jobs: the
// view shows "sent", then the real outcome from the chain - applied, or reverted (still charged).
// Contract: core/src/lez_actors_module_impl.h. Spec: docs/SPEC.md.
Item {
    id: root
    anchors.fill: parent

    readonly property color cBg: Theme.palette.background || "#10130f"
    readonly property color cCard: Theme.palette.backgroundElevated || "#181d16"
    readonly property color cInset: Theme.palette.backgroundInset || "#0b0d0a"
    readonly property color cLine: Theme.palette.borderHairline || "#2a3326"
    readonly property color cText: Theme.palette.text || "#eef2e8"
    readonly property color cText2: Theme.palette.textSecondary || "#c2cbb8"
    readonly property color cText3: Theme.palette.textTertiary || "#8b9682"
    readonly property color cPrimary: Theme.palette.primary || "#8fbf5a"
    readonly property color cOk: Theme.palette.success || "#5fb85a"
    readonly property color cWarn: Theme.palette.warning || "#e0a03a"
    readonly property color cErr: Theme.palette.error || "#e5534b"
    readonly property int sp: Theme.spacing.medium || 12
    readonly property int spS: Theme.spacing.small || 8
    readonly property int rad: Theme.spacing.radiusSmall || 6

    property var st: ({})            // snapshot()
    property var hl: ({})            // health()
    property var accounts: []        // listAccounts().accounts
    property var programs: []        // listKnownPrograms().programs
    property var detail: null        // getAccount(selected)
    property string callProgramId: ""
    property string toastMsg: ""
    property bool toastErr: false
    property var watched: ({})       // jobId -> already reported in a toast
    property var examples: []        // listExamples().examples
    property var chainLog: []        // localChainLog() lines
    property int chainLogSeq: 0
    property bool showLog: false
    property var stepText: ({})      // "example/step" -> edited text
    readonly property var chain: root.st.localChain || ({})
    readonly property bool walletOpen: !!(root.st.wallet && root.st.wallet.open)

    // ── calls ──────────────────────────────────────────────────────────────────
    function callVia(mod, method, args, cb) {
        var a = args || []
        var done = function (raw) { if (cb) { try { cb(raw === undefined || raw === null ? "" : raw) } catch (e) { console.warn(e) } } }
        if (typeof logos === "undefined" || logos === null) { Qt.callLater(function () { done("") }); return }
        if (typeof logos.callModuleAsync === "function") {
            try { logos.callModuleAsync(mod, method, a, done, 20000) } catch (e) { Qt.callLater(function () { done("") }) }
            return
        }
        Qt.callLater(function () { var r = ""; try { r = logos.callModule(mod, method, a) } catch (e) {} done(r) })
    }
    function core(method, args, cb) { root.callVia("lez_actors_module", method, args, cb) }
    function parse(raw) {
        var v = raw
        for (var i = 0; i < 3 && typeof v === "string"; i++) { try { v = JSON.parse(v) } catch (e) { return null } }
        return (v && typeof v === "object") ? v : null
    }
    function toast(msg, err) { root.toastMsg = msg; root.toastErr = !!err; toastTimer.interval = err ? 15000 : 6000; toastTimer.restart() }
    function act(method, args, okMsg, onOk) {
        root.core(method, args, function (raw) {
            var r = root.parse(raw)
            if (!r) { root.toast("Request failed - is lez_actors_module loaded?", true); return }
            if (r.status !== "ok") { root.toast(r.error || "Failed", true); return }
            if (okMsg) root.toast(okMsg, false)
            if (onOk) onOk(r)
            root.refresh()
        })
    }
    // raw units, grouped the same way on every machine (no host locale)
    function units(s) {
        s = String(s || "0"); var out = ""
        for (var i = 0; i < s.length; i++) { if (i > 0 && (s.length - i) % 3 === 0) out += " "; out += s[i] }
        return out
    }
    function shortId(id) { id = String(id || ""); return id.length > 14 ? id.slice(0, 6) + "…" + id.slice(-6) : id }
    function programLabel(pid) { for (var i = 0; i < root.programs.length; i++) if (root.programs[i].programId === pid) return root.programs[i].name; return "" }

    property bool busy: false
    property bool again: false
    function refresh() {
        if (root.busy) { root.again = true; return }
        root.busy = true
        busyGuard.restart()
        var sel = root.st.selected || ""
        var walletOpen = !!(root.st.wallet && root.st.wallet.open)
        var guided = tabs.currentIndex === 0
        var pending = 3 + (walletOpen ? 1 : 0) + (sel ? 1 : 0) + (guided ? 1 : 0) + (root.showLog ? 1 : 0)
        var fin = function () { if (--pending > 0) return; root.busy = false; if (root.again) { root.again = false; root.refresh() } }
        root.core("snapshot", [], function (raw) { var s = root.parse(raw); if (s && s.status === "ok") { root.st = s; root.reportJobs(s.jobs || []) } fin() })
        root.core("health", [], function (raw) { var h = root.parse(raw); if (h) root.hl = h; fin() })
        root.core("listKnownPrograms", [], function (raw) { var p = root.parse(raw); if (p && p.status === "ok") root.programs = p.programs; fin() })
        if (walletOpen) root.core("listAccounts", [], function (raw) { var a = root.parse(raw); if (a && a.status === "ok") root.accounts = a.accounts; fin() })
        if (sel) root.core("getAccount", [sel], function (raw) { var d = root.parse(raw); if (d && d.status === "ok") root.detail = d; fin() })
        if (guided) root.core("listExamples", [], function (raw) { var e = root.parse(raw); if (e && e.status === "ok") root.examples = e.examples; fin() })
        if (root.showLog) root.core("localChainLog", [String(root.chainLogSeq)], function (raw) {
            var l = root.parse(raw)
            if (l && l.status === "ok" && l.lines.length) {
                var all = root.chainLog.concat(l.lines)
                root.chainLog = all.slice(Math.max(0, all.length - 300))
                root.chainLogSeq = l.lastSeq
            }
            fin()
        })
    }
    // a finished job is reported once: applied in green, reverted / failed in red - never a
    // success toast for a transaction that reverted
    function reportJobs(jobs) {
        for (var i = 0; i < jobs.length; i++) {
            var j = jobs[i]
            if (["applied", "reverted", "failed", "unknown"].indexOf(j.state) < 0 || root.watched[j.jobId]) continue
            var w = root.watched; w[j.jobId] = true; root.watched = w
            if (!j.finishedMs || Date.now() - j.finishedMs > 120000) continue   // old, from before this view opened
            var what = root.jobTitle(j)
            if (j.state === "applied") root.toast(what + " applied" + (j.result && j.result.fee ? " (fee " + root.units(j.result.fee) + ")" : ""), false)
            else if (j.state === "reverted") root.toast(what + " reverted: " + ((j.result && j.result.revertReason) || "reason unavailable"), true)
            else root.toast(what + " failed: " + (j.error || "unknown"), true)
        }
    }
    function jobTitle(j) {
        if (j.kind === "example") return j.request.example + ": " + j.request.label
        if (j.kind === "deploy") return "Deploy " + j.request.example
        return j.kind === "call" ? "Call" : "Transfer"
    }
    // the latest job of an example step, or null
    function lastStepJob(ex, step) {
        var jobs = root.st.jobs || []
        for (var i = 0; i < jobs.length; i++) if (jobs[i].kind === "example" && jobs[i].request.example === ex && jobs[i].request.step === step) return jobs[i]
        return null
    }
    function lastDeployJob(ex) {
        var jobs = root.st.jobs || []
        for (var i = 0; i < jobs.length; i++) if (jobs[i].kind === "deploy" && jobs[i].request.example === ex) return jobs[i]
        return null
    }
    function payerBalance() {
        for (var i = 0; i < root.accounts.length; i++) if (root.accounts[i].accountId === root.st.payer) return root.accounts[i].onChain ? root.units(root.accounts[i].balance) + " raw units" : "off-chain (fund it, or use the dev accounts)"
        return ""
    }
    function accountLabel(id) {
        var ex = root.st.exampleAccounts || []
        if (id === ex[0]) return "example account 1"
        if (id === ex[1]) return "example account 2"
        return root.shortId(id)
    }
    function stateColor(state) { return state === "applied" ? root.cOk : (["reverted", "failed", "unknown"].indexOf(state) >= 0 ? root.cErr : root.cWarn) }
    function bytesText(hex, text) { return !hex ? "(empty)" : text !== undefined ? "“" + text + "”" : "0x" + hex }
    function payerFunded() {
        for (var i = 0; i < root.accounts.length; i++) if (root.accounts[i].accountId === root.st.payer) return root.accounts[i].onChain && root.accounts[i].balance !== "0"
        return false
    }
    Timer { id: busyGuard; interval: 30000; onTriggered: root.busy = false }
    Timer { interval: 3000; running: true; repeat: true; onTriggered: root.refresh() }
    Timer { id: toastTimer; interval: 6000; onTriggered: root.toastMsg = "" }
    Connections {
        target: typeof logos !== "undefined" ? logos : null
        ignoreUnknownSignals: true
        function onModuleEventReceived(module, event, data) { if (module === "lez_actors_module" && event === "stateChanged") root.refresh() }
    }
    Component.onCompleted: {
        if (typeof logos !== "undefined" && logos && logos.onModuleEvent) logos.onModuleEvent("lez_actors_module", "stateChanged")
        root.refresh()
    }

    // ── components ─────────────────────────────────────────────────────────────
    component T1: Text { textFormat: Text.PlainText; color: root.cText; font.pixelSize: 15; font.weight: Font.DemiBold; wrapMode: Text.Wrap }
    component T2: Text { textFormat: Text.PlainText; color: root.cText2; font.pixelSize: 13; wrapMode: Text.Wrap }
    component T3: Text { textFormat: Text.PlainText; color: root.cText3; font.pixelSize: 11; wrapMode: Text.Wrap }
    component Mono: TextEdit { readOnly: true; selectByMouse: true; textFormat: TextEdit.PlainText; color: root.cText2; font.family: "monospace"; font.pixelSize: 12; wrapMode: TextEdit.WrapAnywhere; Layout.fillWidth: true }
    component Card: Rectangle {
        default property alias content: inner.data
        Layout.fillWidth: true
        implicitHeight: inner.implicitHeight + 2 * root.sp
        color: root.cCard; radius: root.rad + 4; border.color: root.cLine
        ColumnLayout { id: inner; anchors.fill: parent; anchors.margins: root.sp; spacing: root.spS }
    }
    component Field: TextField {
        Layout.fillWidth: true; Layout.minimumWidth: 80
        color: root.cText; placeholderTextColor: root.cText3; font.pixelSize: 13
        background: Rectangle { color: root.cInset; radius: root.rad; border.color: parent.activeFocus ? root.cPrimary : root.cLine }
    }

    Rectangle { anchors.fill: parent; color: root.cBg }

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth
        ColumnLayout {
            width: parent.width - 2 * root.sp
            x: root.sp
            spacing: root.sp

            Item { Layout.preferredHeight: root.spS }
            RowLayout {
                T1 { text: "LEZ Actors"; font.pixelSize: 20 }
                T3 { text: "a playground: an experimental app to try out LEZ v0.4 actor programs (may change or go away) - built on the Logos tech stack, not affiliated with Logos"; Layout.fillWidth: true }
            }

            // ── connect bar ──
            Card {
                objectName: "connectBar"
                border.color: root.hl.reachable ? root.cLine : root.cErr
                RowLayout {
                    Layout.fillWidth: true
                    T2 { text: "Sequencer" }
                    Field { id: rpcField; objectName: "rpcField"; placeholderText: "http://127.0.0.1:3040"; Component.onCompleted: text = root.st.rpcUrl || "http://127.0.0.1:3040" }
                    LogosButton { text: "Connect"; onClicked: root.act("setEndpoint", [rpcField.text.trim()], "Sequencer set") }
                }
                T2 {
                    Layout.fillWidth: true
                    color: root.hl.reachable ? root.cText2 : root.cErr
                    text: root.hl.reachable
                          ? "Connected - block " + root.hl.blockHeight + "  -  base fees: execution " + root.hl.baseFeeExec + ", storage " + root.hl.baseFeeStor + " (raw units per gas / per byte)"
                          : "Not reachable: " + (root.hl.error || "...") + "\nStart the local chain in Guided below, or run a v0.4 sequencer yourself (LEZ dev, standalone)."
                }
            }

            // ── wallet ──
            Card {
                objectName: "walletCard"
                visible: !(root.st.wallet && root.st.wallet.open)
                T1 { text: root.st.wallet && root.st.wallet.exists ? "Unlock your wallet" : "Create a wallet" }
                T2 { Layout.fillWidth: true; text: root.st.wallet && root.st.wallet.exists ? "The wallet's keys are stored encrypted under your password." : "A LEZ v0.4 wallet that signs your calls and transfers and pays their fees. Its keys are stored encrypted under the password." }
                RowLayout {
                    Layout.fillWidth: true
                    Field { id: pw; echoMode: TextInput.Password; placeholderText: "password" }
                    LogosButton {
                        text: root.st.wallet && root.st.wallet.exists ? "Unlock" : "Create"
                        enabled: pw.text.length > 0
                        onClicked: {
                            var exists = root.st.wallet && root.st.wallet.exists
                            root.act(exists ? "unlockWallet" : "createWallet", [pw.text], exists ? "Wallet unlocked" : "Wallet created", function (r) {
                                if (r.mnemonic) mnemonicBox.text = r.mnemonic
                            })
                            pw.text = ""
                        }
                    }
                }
            }
            Card {
                visible: mnemonicBox.text !== ""
                border.color: root.cWarn
                T1 { text: "Recovery phrase - write it down, it is shown once" }
                Mono { id: mnemonicBox; text: "" }
                LogosButton { text: "I wrote it down"; onClicked: mnemonicBox.text = "" }
            }

            TabBar {
                id: tabs
                Layout.fillWidth: true
                currentIndex: 0
                onCurrentIndexChanged: root.refresh()
                TabButton { text: "Guided: deploy and run the v0.4 examples" }
                TabButton { text: "Explorer: accounts, any program, transfers" }
            }

            // ── Guided: the wizard ─────────────────────────────────────────────────
            ColumnLayout {
                objectName: "guided"
                visible: tabs.currentIndex === 0
                Layout.fillWidth: true
                spacing: root.sp

                // 1. the local chain
                Card {
                    objectName: "chainCard"
                    border.color: root.chain.state === "running" || root.chain.state === "external" ? root.cOk : root.chain.state === "failed" ? root.cErr : root.cLine
                    RowLayout {
                        Layout.fillWidth: true
                        T1 { text: "1. A local v0.4 chain"; Layout.fillWidth: true }
                        T2 {
                            text: root.chain.state === "running" ? "running in this app, on 127.0.0.1:3040"
                                  : root.chain.state === "external" ? "using the sequencer already on 127.0.0.1:3040"
                                  : root.chain.state || "stopped"
                            color: root.chain.state === "running" || root.chain.state === "external" ? root.cOk : root.chain.state === "failed" ? root.cErr : root.cWarn
                        }
                    }
                    T2 {
                        Layout.fillWidth: true
                        text: "A single-node LEZ sequencer (LEZ dev, the actor model) running inside this app: no Docker, no Bedrock node, nothing to download. "
                              + "Guests run without real proofs (RISC0_DEV_MODE), and its genesis funds two dev accounts. A block every 3 seconds. Its data stays in the app's folder until you reset it."
                    }
                    T2 { visible: !!root.chain.error; Layout.fillWidth: true; color: root.cErr; text: root.chain.error || "" }
                    RowLayout {
                        LogosButton { text: "Start"; enabled: ["stopped", "failed", undefined].indexOf(root.chain.state) >= 0; onClicked: root.act("startLocalChain", [], "Starting the local chain...") }
                        LogosButton { text: "Stop"; enabled: root.chain.state === "running" || root.chain.state === "external"; onClicked: root.act("stopLocalChain", [], "Stopping") }
                        LogosButton { text: "Reset (fresh genesis)"; enabled: root.chain.state !== "starting" && root.chain.state !== "stopping" && root.chain.state !== "external"; onClicked: root.act("resetLocalChain", [], "Deleting the local chain; start it again for a fresh one") }
                        LogosButton { text: root.showLog ? "Hide log" : "Show log"; onClicked: { root.showLog = !root.showLog; root.refresh() } }
                        T3 { Layout.fillWidth: true; text: root.hl.reachable ? "block " + root.hl.blockHeight : "" }
                    }
                    Rectangle {
                        visible: root.showLog
                        Layout.fillWidth: true
                        Layout.preferredHeight: 260
                        color: root.cInset; radius: root.rad; border.color: root.cLine
                        ScrollView {
                            id: logScroll
                            anchors.fill: parent; anchors.margins: 6
                            clip: true
                            Mono {
                                id: logText
                                width: logScroll.availableWidth
                                font.pixelSize: 11
                                text: root.chainLog.length ? root.chainLog.map(function (l) { return l.text }).join("\n") : "(nothing yet)"
                                onTextChanged: Qt.callLater(function () { if (logScroll.ScrollBar.vertical) logScroll.ScrollBar.vertical.position = 1.0 - logScroll.ScrollBar.vertical.size })
                            }
                        }
                    }
                }

                // 2. the wallet (the card above does the work)
                Card {
                    T1 { text: "2. A wallet" }
                    T2 {
                        Layout.fillWidth: true
                        color: root.walletOpen ? root.cOk : root.cText2
                        text: root.walletOpen ? "Unlocked. It signs and pays for everything below."
                                              : "Create (or unlock) a wallet in the card above. It holds the keys that sign and pay; in v0.4 every transaction declares a fee and its payer signs."
                    }
                }

                // 3. the dev accounts
                Card {
                    T1 { text: "3. Funded accounts" }
                    T2 {
                        Layout.fillWidth: true
                        text: "The dev genesis funds two public accounts (LEZ's own test keys). Importing them gives the wallet something to pay fees with; the 20 T one becomes the payer. "
                              + "The examples then run on two fresh accounts of their own (example accounts 1 and 2), which never pay, so whether they sign is up to each step."
                    }
                    RowLayout {
                        LogosButton { text: "Use the dev accounts"; enabled: root.walletOpen && root.hl.reachable; onClicked: root.act("useDevAccounts", [], "Dev accounts imported; the 20 T one pays") }
                        T3 {
                            Layout.fillWidth: true
                            text: root.st.payer ? "payer " + root.shortId(root.st.payer) + (root.accounts.length ? "  -  " + root.payerBalance() : "") : "no payer yet"
                        }
                    }
                    T3 {
                        visible: (root.st.exampleAccounts || []).length > 0
                        Layout.fillWidth: true
                        text: "example account 1: " + ((root.st.exampleAccounts || [])[0] || "") + "\nexample account 2: " + ((root.st.exampleAccounts || [])[1] || "")
                        font.family: "monospace"
                    }
                }

                // 4. the examples
                T1 { text: "4. The examples: LEZ's program-deployment tutorial programs"; Layout.topMargin: root.spS }
                T2 {
                    Layout.fillWidth: true
                    text: "Each is a real RISC Zero guest from LEZ (examples/program_deployment), built into this app. Deploy uploads it through the program loader: the bytecode goes into segment accounts and a header account points at them; "
                          + "the header's id is the program's id. Every step says what it should do; the result shows each mentioned actor's state before and after, the fee, and, for a revert, the guest's panic message from the chain's log."
                }
                Repeater {
                    model: root.examples
                    delegate: Card {
                        id: exCard
                        required property var modelData
                        readonly property var deployJob: root.lastDeployJob(modelData.name)
                        readonly property bool deploying: !!deployJob && ["applied", "failed", "reverted", "unknown"].indexOf(deployJob.state) < 0
                        border.color: modelData.deployed ? root.cLine : root.cWarn
                        RowLayout {
                            Layout.fillWidth: true
                            T1 { text: exCard.modelData.title; Layout.fillWidth: true }
                            T3 { text: exCard.modelData.deployed ? "deployed: " + exCard.modelData.programId : Math.round(exCard.modelData.elfSize / 1024) + " KB guest"; font.family: "monospace" }
                        }
                        T2 { Layout.fillWidth: true; text: exCard.modelData.about }
                        T3 { visible: exCard.modelData.needs.length > 0; Layout.fillWidth: true; text: "calls " + exCard.modelData.needs.join(", ") + ": deploying this deploys that too" }
                        RowLayout {
                            visible: !exCard.modelData.deployed
                            LogosButton {
                                text: exCard.deploying ? "Deploying..." : "Deploy"
                                enabled: !exCard.deploying && root.walletOpen && root.payerFunded() && root.hl.reachable
                                onClicked: root.act("deployExample", [exCard.modelData.name], "Deploying " + exCard.modelData.name + " (a few blocks)")
                            }
                            T3 {
                                Layout.fillWidth: true
                                color: exCard.deployJob && exCard.deployJob.state === "failed" ? root.cErr : root.cText3
                                text: exCard.deployJob ? (exCard.deployJob.state === "failed" ? exCard.deployJob.error : exCard.deployJob.state) : (root.payerFunded() ? "fee paid by " + root.shortId(root.st.payer) : "needs a funded payer (step 3)")
                            }
                        }
                        Repeater {
                            model: exCard.modelData.deployed ? exCard.modelData.steps : []
                            delegate: Rectangle {
                                id: stepBox
                                required property var modelData
                                readonly property string key: exCard.modelData.name + "/" + modelData.id
                                readonly property var job: root.lastStepJob(exCard.modelData.name, modelData.id)
                                readonly property bool running: !!job && ["applied", "failed", "reverted", "unknown"].indexOf(job.state) < 0
                                Layout.fillWidth: true
                                implicitHeight: stepCol.implicitHeight + 16
                                color: root.cInset; radius: root.rad; border.color: root.cLine
                                ColumnLayout {
                                    id: stepCol
                                    anchors.fill: parent; anchors.margins: 8
                                    spacing: 4
                                    RowLayout {
                                        Layout.fillWidth: true
                                        T2 { text: stepBox.modelData.label; color: root.cText; font.weight: Font.DemiBold; Layout.fillWidth: true }
                                        T3 { text: "expected: " + stepBox.modelData.expect; color: stepBox.modelData.expect === "applied" ? root.cOk : root.cErr }
                                    }
                                    T3 { Layout.fillWidth: true; text: stepBox.modelData.explain }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Field {
                                            id: stepField
                                            visible: stepBox.modelData.takesText
                                            placeholderText: "message text"
                                            Component.onCompleted: text = root.stepText[stepBox.key] !== undefined ? root.stepText[stepBox.key] : stepBox.modelData.defaultText
                                            onTextChanged: { var t = root.stepText; t[stepBox.key] = text; root.stepText = t }
                                        }
                                        Item { visible: !stepBox.modelData.takesText; Layout.fillWidth: true }
                                        LogosButton {
                                            text: stepBox.running ? "Running..." : "Run"
                                            enabled: !stepBox.running && root.walletOpen && !!root.st.payer
                                            onClicked: root.act("runExampleStep", [JSON.stringify({ example: exCard.modelData.name, step: stepBox.modelData.id, text: stepField.text })], "Sent - waiting for the chain")
                                        }
                                    }
                                    // the outcome
                                    RowLayout {
                                        visible: !!stepBox.job
                                        Layout.fillWidth: true
                                        T2 { text: stepBox.job ? stepBox.job.state : ""; color: root.stateColor(stepBox.job ? stepBox.job.state : "") }
                                        T3 {
                                            visible: !!(stepBox.job && stepBox.job.result && stepBox.job.result.asExpected !== undefined)
                                            text: stepBox.job && stepBox.job.result && stepBox.job.result.asExpected ? "as expected" : "NOT what was expected"
                                            color: stepBox.job && stepBox.job.result && stepBox.job.result.asExpected ? root.cOk : root.cErr
                                        }
                                        T3 { visible: !!(stepBox.job && stepBox.job.result && stepBox.job.result.fee); text: stepBox.job && stepBox.job.result ? "fee " + root.units(stepBox.job.result.fee) : "" }
                                        Item { Layout.fillWidth: true }
                                    }
                                    T3 { visible: !!(stepBox.job && stepBox.job.error); Layout.fillWidth: true; color: root.cErr; text: stepBox.job ? stepBox.job.error || "" : "" }
                                    T3 { visible: !!(stepBox.job && stepBox.job.result && stepBox.job.result.revertReason); Layout.fillWidth: true; color: root.cErr; text: stepBox.job && stepBox.job.result ? "revert: " + (stepBox.job.result.revertReason || "") : "" }
                                    Repeater {
                                        model: stepBox.job && stepBox.job.result && stepBox.job.result.actors ? stepBox.job.result.actors : []
                                        delegate: T3 {
                                            required property var modelData
                                            required property int index
                                            Layout.fillWidth: true
                                            color: modelData.changed ? root.cText : root.cText3
                                            text: (index === 0 ? "root actor  " : "mentioned   ") + "(" + root.accountLabel(modelData.accountId) + ", " + (modelData.programName || root.shortId(modelData.programId)) + ")"
                                                  + (modelData.signs ? "  signs" : "") + "\n    " + root.bytesText(modelData.before, modelData.beforeText) + "  →  " + root.bytesText(modelData.after, modelData.afterText)
                                            font.family: "monospace"
                                        }
                                    }
                                    T3 { visible: !!(stepBox.job && stepBox.job.result && stepBox.job.result.messageHex); Layout.fillWidth: true; text: stepBox.job && stepBox.job.result ? "message (borsh) 0x" + stepBox.job.result.messageHex : ""; font.family: "monospace"; wrapMode: Text.WrapAnywhere }
                                    T3 { visible: !!(stepBox.job && stepBox.job.txHash); Layout.fillWidth: true; text: stepBox.job ? "tx " + stepBox.job.txHash : ""; font.family: "monospace" }
                                }
                            }
                        }
                    }
                }
            }

            GridLayout {
                id: panes
                // two columns when there is room, stacked otherwise
                readonly property bool twoCol: width >= 1000
                Layout.fillWidth: true
                columns: twoCol ? 2 : 1
                columnSpacing: root.sp; rowSpacing: root.sp
                visible: tabs.currentIndex === 1 && root.walletOpen

                // ── left: accounts + programs ──
                // a nested layout fills by default and its implicit width (long placeholders)
                // would squeeze the right column to nothing: pin it when side by side
                ColumnLayout {
                    Layout.fillWidth: !panes.twoCol
                    Layout.preferredWidth: panes.twoCol ? 520 : -1
                    Layout.maximumWidth: panes.twoCol ? 520 : Number.POSITIVE_INFINITY
                    Layout.alignment: Qt.AlignTop
                    spacing: root.sp
                    Card {
                        objectName: "accountsPane"
                        RowLayout {
                            Layout.fillWidth: true
                            T1 { text: "Accounts"; Layout.fillWidth: true }
                            LogosButton { text: "New account"; onClicked: root.act("newAccount", [], "Account created (off-chain until something writes to it)") }
                        }
                        T3 { Layout.fillWidth: true; text: "★ pays fees   ▶ shown below   balances in raw units" }
                        Repeater {
                            model: root.accounts
                            delegate: Rectangle {
                                required property var modelData
                                Layout.fillWidth: true
                                implicitHeight: arow.implicitHeight + 8
                                radius: root.rad
                                color: modelData.accountId === root.st.selected ? root.cInset : "transparent"
                                RowLayout {
                                    id: arow
                                    anchors.fill: parent; anchors.margins: 4
                                    T2 { text: (modelData.accountId === root.st.payer ? "★ " : "  ") + (modelData.accountId === root.st.selected ? "▶ " : "  ") + root.shortId(modelData.accountId); font.family: "monospace" }
                                    T2 { Layout.fillWidth: true; horizontalAlignment: Text.AlignRight; text: modelData.onChain ? root.units(modelData.balance) : "off-chain"; color: modelData.onChain ? root.cText : root.cText3 }
                                    LogosButton { text: "Show"; onClicked: root.act("setSelected", [modelData.accountId], "") }
                                    LogosButton { text: "Pay with"; enabled: modelData.accountId !== root.st.payer; onClicked: root.act("setPayer", [modelData.accountId], "Payer set") }
                                }
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Field { id: importField; placeholderText: "import a private key (hex) - e.g. a dev chain's funded test account"; echoMode: TextInput.Password }
                            LogosButton { text: "Import"; enabled: importField.text.length >= 64; onClicked: { root.act("importKey", [importField.text.trim()], "Key imported"); importField.text = "" } }
                        }
                    }
                    Card {
                        objectName: "programsPane"
                        T1 { text: "Programs" }
                        T3 { Layout.fillWidth: true; text: "Built-ins are dimmed. Add a deployed program by its header account id to label its actor states and call it." }
                        Repeater {
                            model: root.programs
                            delegate: RowLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                T2 { text: modelData.name; color: modelData.builtin ? root.cText3 : root.cText; Layout.preferredWidth: 130; elide: Text.ElideRight }
                                T3 { text: root.shortId(modelData.programId); font.family: "monospace"; Layout.fillWidth: true }
                                LogosButton { text: "Call"; visible: !modelData.builtin; onClicked: root.callProgramId = modelData.programId }
                                LogosButton { text: "Remove"; visible: !modelData.builtin; onClicked: root.act("removeProgram", [modelData.programId], "Removed") }
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Field { id: progName; placeholderText: "name"; Layout.preferredWidth: 110; Layout.fillWidth: false }
                            Field { id: progId; placeholderText: "program header id (base58 or hex)" }
                            LogosButton {
                                text: "Add"
                                enabled: progName.text.trim() !== "" && progId.text.trim() !== ""
                                onClicked: root.core("resolveProgram", [progId.text.trim()], function (raw) {
                                    var r = root.parse(raw)
                                    if (!r || r.status !== "ok") { root.toast(r ? r.error : "Request failed", true); return }
                                    if (!r.exists) root.toast("No program is deployed at that id (yet) - added anyway, check the id", true)
                                    root.act("addProgram", [JSON.stringify({ name: progName.text.trim(), programId: progId.text.trim() })], r.exists ? "Program added" : "", function () { progName.text = ""; progId.text = "" })
                                })
                            }
                        }
                    }
                }

                // ── right: account detail, call, transfer, activity ──
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 360
                    Layout.alignment: Qt.AlignTop
                    spacing: root.sp
                    Card {
                        objectName: "accountDetail"
                        T1 { text: "Account" }
                        T2 { visible: !root.detail; text: "Choose \"Show\" on an account." }
                        Mono { visible: !!root.detail; text: root.detail ? root.detail.accountId : "" }
                        T2 {
                            visible: !!root.detail
                            text: root.detail ? (root.detail.onChain ? "nonce " + root.detail.nonce + "  -  balance " + root.units(root.detail.balance) + " raw units" : "not on chain yet: nothing has written to this account") : ""
                        }
                        T3 { visible: !!root.detail && root.detail.actorStates.length > 0; text: "Actor states - one per program that keeps state on this account:" }
                        Repeater {
                            model: root.detail ? root.detail.actorStates : []
                            delegate: Rectangle {
                                required property var modelData
                                Layout.fillWidth: true
                                implicitHeight: srow.implicitHeight + 12
                                color: root.cInset; radius: root.rad; border.color: root.cLine
                                ColumnLayout {
                                    id: srow
                                    anchors.fill: parent; anchors.margins: 6
                                    T2 { text: (modelData.programName || root.shortId(modelData.programId)) + "  (" + modelData.size + " bytes)"; color: root.cText }
                                    Mono {
                                        text: modelData.balance !== undefined ? "balance " + root.units(modelData.balance)
                                              : modelData.text !== undefined ? "“" + modelData.text + "”" : "0x" + modelData.hex
                                    }
                                }
                            }
                        }
                    }
                    Card {
                        objectName: "callForm"
                        T1 { text: "Call a program" }
                        RowLayout {
                            Layout.fillWidth: true
                            T2 { text: "program"; Layout.preferredWidth: 70 }
                            Field { id: callProg; placeholderText: "program id"; text: root.callProgramId }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            T2 { text: "account"; Layout.preferredWidth: 70 }
                            Field { id: callAcct; placeholderText: "account the actor lives on"; text: root.st.selected || "" }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            T2 { text: "message"; Layout.preferredWidth: 70 }
                            ComboBox { id: callEnc; model: ["text", "hex", "raw-text"]; Layout.preferredWidth: 110 }
                            Field { id: callMsg; placeholderText: callEnc.currentText === "hex" ? "borsh-encoded message, hex" : "Hola mundo!" }
                        }
                        T3 { Layout.fillWidth: true; text: callEnc.currentText === "text" ? "text = a borsh Vec<u8> of the text (what hello_world takes); hex = the exact message bytes; raw-text = the text's bytes as they are" : callEnc.currentText === "hex" ? "the exact borsh-encoded message the program expects" : "the text's UTF-8 bytes, unframed" }
                        RowLayout {
                            id: signRow
                            Layout.fillWidth: true
                            readonly property bool isPayer: callAcct.text.trim() !== "" && callAcct.text.trim() === String(root.st.payer || "")
                            // the host style draws CheckBox text too dim on the dark theme: label it ourselves
                            CheckBox { id: callSign; visible: !parent.isPayer }
                            T2 {
                                Layout.fillWidth: true
                                text: parent.isPayer ? "the account signs: it pays the fee, and LEZ has the payer sign"
                                                     : "the account signs too (programs that check is_authorized need it)"
                                MouseArea { anchors.fill: parent; enabled: callSign.visible; onClicked: callSign.checked = !callSign.checked }
                            }
                        }
                        RowLayout {
                            LogosButton {
                                objectName: "sendCall"
                                text: "Send"
                                enabled: callProg.text.trim() !== "" && callAcct.text.trim() !== "" && !!root.st.payer
                                onClicked: root.act("callProgram", [JSON.stringify({ programId: callProg.text.trim(), accountId: callAcct.text.trim(), message: callMsg.text, encoding: callEnc.currentText, sign: callSign.checked || signRow.isPayer })],
                                                    "Sent - waiting for the chain")
                            }
                            T3 { text: root.st.payer ? "fee paid by " + root.shortId(root.st.payer) : "choose a payer in Accounts" }
                        }
                    }
                    Card {
                        objectName: "transferForm"
                        T1 { text: "Native transfer" }
                        RowLayout {
                            Layout.fillWidth: true
                            T2 { text: "from"; Layout.preferredWidth: 70 }
                            Field { id: tFrom; placeholderText: "an account in this wallet"; text: root.st.payer || "" }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            T2 { text: "to"; Layout.preferredWidth: 70 }
                            Field { id: tTo; placeholderText: "recipient account (on chain, or in this wallet)" }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            T2 { text: "amount"; Layout.preferredWidth: 70 }
                            Field { id: tAmount; placeholderText: "raw units, whole number"; validator: RegularExpressionValidator { regularExpression: /[0-9]{0,39}/ } }
                        }
                        RowLayout {
                            LogosButton {
                                text: "Send"
                                enabled: tFrom.text.trim() !== "" && tTo.text.trim() !== "" && tAmount.text !== ""
                                onClicked: root.act("transfer", [JSON.stringify({ from: tFrom.text.trim(), to: tTo.text.trim(), amount: tAmount.text })], "Sent - the credit can take a block or two")
                            }
                            T3 { text: "fee paid by " + root.shortId(root.st.payer || tFrom.text) }
                        }
                    }
                    Card {
                        objectName: "activity"
                        T1 { text: "Activity" }
                        T2 { visible: !(root.st.jobs && root.st.jobs.length); text: "Nothing sent yet." }
                        Repeater {
                            model: root.st.jobs || []
                            delegate: ColumnLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 2
                                RowLayout {
                                    Layout.fillWidth: true
                                    T2 {
                                        text: modelData.kind === "call" ? "call " + root.shortId(modelData.request.programId)
                                              : modelData.kind === "transfer" ? "transfer " + root.units(modelData.request.amount) + " → " + root.shortId(modelData.request.to)
                                              : root.jobTitle(modelData)
                                        color: root.cText; Layout.fillWidth: true
                                    }
                                    T2 {
                                        text: modelData.state
                                        color: root.stateColor(modelData.state)
                                    }
                                }
                                T3 { visible: !!modelData.txHash; text: "tx " + modelData.txHash; font.family: "monospace"; Layout.fillWidth: true }
                                T3 {
                                    visible: (modelData.kind === "call" || modelData.kind === "example") && !!modelData.result && modelData.result.stateAfter !== undefined
                                    Layout.fillWidth: true
                                    text: modelData.result ? "state " + (modelData.result.stateBeforeText !== undefined ? "“" + modelData.result.stateBeforeText + "”" : "0x" + modelData.result.stateBefore)
                                          + " → " + (modelData.result.stateAfterText !== undefined ? "“" + modelData.result.stateAfterText + "”" : "0x" + modelData.result.stateAfter) : ""
                                }
                                T3 { visible: modelData.kind === "transfer" && !!modelData.result && modelData.result.fee !== undefined; Layout.fillWidth: true; text: modelData.result ? "moved " + root.units(modelData.result.moved) + ", fee " + root.units(modelData.result.fee) : "" }
                                T3 { visible: !!(modelData.result && modelData.result.revertReason) || !!modelData.error; Layout.fillWidth: true; color: root.cErr; text: (modelData.result && modelData.result.revertReason) || modelData.error || "" }
                            }
                        }
                    }
                }
            }
            Item { Layout.preferredHeight: 40 }
        }
    }

    Rectangle {
        visible: root.toastMsg !== ""
        anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom; anchors.bottomMargin: 16
        width: Math.min(parent.width - 40, toastT.implicitWidth + 32); height: toastT.implicitHeight + 16
        radius: root.rad + 2; color: root.toastErr ? "#3a1513" : "#1b2a17"; border.color: root.toastErr ? root.cErr : root.cOk
        Text { id: toastT; anchors.centerIn: parent; width: parent.width - 32; text: root.toastMsg; color: root.cText; wrapMode: Text.Wrap; textFormat: Text.PlainText; font.pixelSize: 13 }
    }
}
