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
    property var sources: ({})       // program source name -> {source, origin}
    function loadSource(name) {
        if (!name || root.sources[name]) return
        root.core("programSource", [name], function (raw) {
            var r = root.parse(raw)
            if (r && r.status === "ok") { var c = root.sources; c[name] = { source: r.source, origin: r.origin }; root.sources = c }
        })
    }
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
    // copy to the clipboard through a hidden text edit (QML has no clipboard API of its own)
    function copyText(t) { clip.text = String(t || ""); clip.selectAll(); clip.copy(); clip.text = ""; root.toast("Copied " + root.shortId(t), false) }
    function programInfo(pid) {
        for (var i = 0; i < root.programs.length; i++) if (root.programs[i].programId === pid) return root.programs[i]
        return null
    }
    function programKind(pid) {
        var p = root.programInfo(pid)
        if (!p) return "a program not in your list (add it in Programs to name it)"
        if (p.name === "native token") return "built into the engine"
        if (p.name === "program_loader") return "built into the engine"
        return p.builtin ? "a built-in program (LEZ's own guests)" : "a program you deployed or added"
    }
    // what this program keeps in its actor state, as far as this app knows
    function stateMeaning(name) {
        var m = {
            "native token": "This account's native-token balance: a 16-byte little-endian number. A transfer rewrites it on the sender and on the recipient.",
            "program_loader": "Program bytecode or a program header: this account is part of a deployed program, not a user account.",
            "hello_world": "The bytes hello_world appended here, one call after another. Only hello_world can write this slot.",
            "hello_world_with_authorization": "The bytes appended by calls this account authorized (signed). An unsigned call reverts and leaves it as it was.",
            "hello_world_with_move_function": "Bytes written here, or moved here from another account's slot under the same program. A move empties the source's slot.",
            "simple_tail_call": "simple_tail_call keeps its own state unchanged: its effect shows up in the callee's slot (hello_world's) instead.",
            "actor_conversation": "The last few things actor_conversation did on this account: balances it read back from the native token (sync request/reply), pings it sent, and pings it received (async)."
        }
        return m[name] || "Bytes that program chose to keep for this account. Only that program can change them, by being called on this actor."
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
    // the v0.4 model in a few lines: what an account, an actor and an actor state are
    component ActorExplainer: Rectangle {
        id: explainer
        property bool open: false
        Layout.fillWidth: true
        implicitHeight: exCol.implicitHeight + 16
        color: root.cInset; radius: root.rad; border.color: root.cLine
        ColumnLayout {
            id: exCol
            anchors.fill: parent; anchors.margins: 8
            spacing: 4
            RowLayout {
                Layout.fillWidth: true
                T2 { text: "What is an actor?"; color: root.cText; font.weight: Font.DemiBold; Layout.fillWidth: true }
                LogosButton { text: explainer.open ? "Hide" : "Explain"; onClicked: explainer.open = !explainer.open }
            }
            T3 {
                Layout.fillWidth: true
                text: "In LEZ v0.4 an account doesn't hold one blob of data. It holds one slot per program, and an actor is that pair: (account, program)."
            }
            T2 {
                visible: explainer.open
                Layout.fillWidth: true
                text: "• Account: an address (32 bytes, shown in base58) with a nonce. By itself it holds nothing else.\n"
                    + "• Actor: (account, program). Every program that has written to an account owns one slot on it: the actor state. Only that program can change its slot, and many programs can keep slots on the same account side by side.\n"
                    + "• Your balance is no exception: it's the native-token program's actor state on your account, a 16-byte number.\n"
                    + "• Calling a program means sending a message to an actor. The program runs with that actor's current bytes (pre_state) and returns the new bytes, plus any messages it wants to send on to other actors.\n"
                    + "• A synchronous message runs right away, inside the sender's handling (depth-first). There is no return value: to get data back, the receiver sends a reply message, and the sender's program runs again to handle it.\n"
                    + "• An asynchronous message to a public actor runs after the synchronous work, still in the same transaction, and carries no authorization. Its destination must be declared in the transaction, or the engine refuses it. (Only private accounts and other zones get async messages later, through a mailbox.) A native transfer is one: Transfer debits the sender and sends Credit to the recipient asynchronously.\n"
                    + "• One transaction is all or nothing: if any step panics, every step is undone.\n"
                    + "• Every actor a call touches is mentioned in the transaction, and each mention says whether its account signs. A program sees that as is_authorized, and can refuse to act without it.\n"
                    + "• Fees: a payer account declares and pays the fee and always signs. A call that reverts (the program panics) is still included and charged, with the whole declared gas limit, and leaves every actor state unchanged.\n"
                    + "• Compared with v0.2/v0.3, where a program owned an account outright: now no program owns the account, each owns only its slot."
            }
        }
    }
    // The actors a step touched, as a table: accounts across, programs down, one cell per actor
    // (account, program) with its state before -> after, and the step's messages as numbered
    // arrows between the cells (solid: sync, dashed: async, from the transaction for the first).
    component ActorMap: Item {
        id: amap
        property var flow: []
        property var actors: []
        property string jobState: ""
        property int focusHop: 0          // 0: every message alike; n: message n stands out
        onFocusHopChanged: canvas.requestPaint()
        readonly property var cols: {
            var out = [], seen = {}
            for (var i = 0; i < flow.length; i++) for (var k of ["to", "from"]) {
                var a = flow[i][k]; if (!a || seen[a.accountId]) continue
                seen[a.accountId] = true; out.push({ id: a.accountId, label: a.accountLabel })
            }
            return out
        }
        readonly property var rows: {
            var out = [], seen = {}
            for (var i = 0; i < flow.length; i++) for (var k of ["to", "from"]) {
                var a = flow[i][k]; if (!a || seen[a.programId]) continue
                seen[a.programId] = true; out.push({ id: a.programId, label: a.programName })
            }
            return out
        }
        readonly property int labelW: 120
        readonly property int txH: 74
        readonly property int cellH: 84
        readonly property int gapX: 120
        readonly property int gapY: 70
        readonly property real cellW: cols.length ? Math.min(420, (width - labelW - gapX * (cols.length - 1)) / cols.length) : 0
        implicitHeight: txH + rows.length * cellH + (rows.length - 1) * gapY + 12
        function colOf(id) { for (var i = 0; i < cols.length; i++) if (cols[i].id === id) return i; return -1 }
        function rowOf(id) { for (var i = 0; i < rows.length; i++) if (rows[i].id === id) return i; return -1 }
        function cellAt(c, r) { return { x: labelW + c * (cellW + gapX), y: txH + r * (cellH + gapY), w: cellW, h: cellH } }
        function cellRect(a) {
            if (!a) {   // the transaction: right above the actor its root message goes to
                var to = flow.length ? flow[0].to : null
                var t = to ? cellAt(colOf(to.accountId), 0) : cellAt(0, 0)
                return { x: t.x + t.w / 2 - 110, y: 2, w: 220, h: 30 }
            }
            return cellAt(colOf(a.accountId), rowOf(a.programId))
        }
        function stateOf(accountId, programId) {
            for (var i = 0; i < actors.length; i++) if (actors[i].accountId === accountId && actors[i].programId === programId) return actors[i]
            return null
        }
        function lastLine(t) { var l = String(t).split("\n"); return l[l.length - 1] }
        onFlowChanged: canvas.requestPaint()
        onWidthChanged: canvas.requestPaint()
        onJobStateChanged: canvas.requestPaint()

        Rectangle {
            readonly property var r: amap.cellRect(null)
            x: r.x; y: r.y; width: r.w; height: r.h; radius: root.rad
            color: root.cCard; border.color: root.cText3
            T3 { anchors.centerIn: parent; text: "the transaction (signed, fee paid)"; color: root.cText2 }
        }
        Repeater {
            model: amap.rows
            delegate: T3 {
                required property var modelData
                required property int index
                readonly property var r: amap.cellAt(0, index)
                x: 0; y: r.y + r.h / 2 - height / 2; width: amap.labelW - 10
                text: "program\n" + modelData.label; color: root.cText2
            }
        }
        Repeater {
            model: amap.rows.length * amap.cols.length
            delegate: Rectangle {
                id: cell
                required property int index
                readonly property var account: amap.cols[index % amap.cols.length]
                readonly property var program: amap.rows[Math.floor(index / amap.cols.length)]
                readonly property var st: account && program ? amap.stateOf(account.id, program.id) : null
                readonly property bool touched: {
                    for (var i = 0; i < amap.flow.length; i++) {
                        var t = amap.flow[i].to, f = amap.flow[i].from
                        if ((t && t.accountId === account.id && t.programId === program.id) || (f && f.accountId === account.id && f.programId === program.id)) return true
                    }
                    return false
                }
                // the payer's balance moves by the fee either way: not the step's effect
                readonly property bool feeOnly: !!st && !!st.note
                readonly property bool changed: !!st && st.changed && amap.jobState === "applied" && !feeOnly
                readonly property var r: amap.cellAt(index % amap.cols.length, Math.floor(index / amap.cols.length))
                x: r.x; y: r.y; width: r.w; height: r.h; radius: root.rad + 2
                color: touched ? root.cInset : "transparent"
                // the actor the focused message is delivered to
                readonly property bool receiving: {
                    if (amap.focusHop < 1 || amap.focusHop > amap.flow.length) return false
                    var t = amap.flow[amap.focusHop - 1].to
                    return !!t && t.accountId === account.id && t.programId === program.id
                }
                border.color: receiving ? root.cPrimary : !touched ? root.cLine : changed ? root.cOk : root.cText3
                border.width: receiving ? 3 : changed ? 2 : 1
                opacity: touched ? 1 : 0.3
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 6
                    spacing: 1
                    T3 { text: touched ? "actor (" + cell.account.label + ", " + cell.program.label + ")" : "(" + cell.account.label + ", " + cell.program.label + "): not in this step"; color: root.cText; font.weight: Font.DemiBold; Layout.fillWidth: true; elide: Text.ElideRight; wrapMode: Text.NoWrap }
                    T3 {
                        visible: !!cell.st
                        Layout.fillWidth: true; elide: Text.ElideRight; wrapMode: Text.NoWrap; font.family: "monospace"
                        text: cell.st ? "before: " + amap.lastLine(root.bytesText(cell.st.before, cell.st.beforeText)) : ""
                    }
                    T3 {
                        visible: !!cell.st
                        Layout.fillWidth: true; elide: Text.ElideRight; wrapMode: Text.NoWrap; font.family: "monospace"
                        color: cell.changed ? root.cOk : root.cText3
                        text: !cell.st ? "" : cell.feeOnly ? "after:  " + amap.lastLine(root.bytesText(cell.st.after, cell.st.afterText)) + "  (only the fee)"
                              : amap.jobState !== "applied" ? "after:  unchanged (reverted)"
                              : cell.st.changed ? "after:  " + amap.lastLine(root.bytesText(cell.st.after, cell.st.afterText)) : "after:  unchanged"
                    }
                    Item { Layout.fillHeight: true }
                }
            }
        }
        Canvas {
            id: canvas
            anchors.fill: parent
            onPaint: {
                var ctx = getContext("2d")
                ctx.reset()
                var fl = amap.flow
                function clip(a, b, rc) {   // where the line a->b leaves the rectangle around a
                    var t = 1, ex = b.x - a.x, ey = b.y - a.y
                    if (ex !== 0) { var tx = ((ex > 0 ? rc.x + rc.w : rc.x) - a.x) / ex; if (tx > 0) t = Math.min(t, tx) }
                    if (ey !== 0) { var ty = ((ey > 0 ? rc.y + rc.h : rc.y) - a.y) / ey; if (ty > 0) t = Math.min(t, ty) }
                    return { x: a.x + ex * t, y: a.y + ey * t }
                }
                for (var i = 0; i < fl.length; i++) {
                    var m = fl[i]
                    var fr = amap.cellRect(m.from), tr = amap.cellRect(m.to)
                    var p = { x: fr.x + fr.w / 2, y: fr.y + fr.h / 2 }, q = { x: tr.x + tr.w / 2, y: tr.y + tr.h / 2 }
                    // a request and its reply run between the same two cells: move each line aside
                    var dx = q.x - p.x, dy = q.y - p.y, len = Math.max(1, Math.sqrt(dx * dx + dy * dy))
                    var nx = -dy / len, ny = dx / len, off = m.kind === "root" ? 0 : 18
                    p = { x: p.x + nx * off, y: p.y + ny * off }; q = { x: q.x + nx * off, y: q.y + ny * off }
                    var s0 = clip(p, q, fr), e0 = clip(q, p, tr)
                    var bad = m.status === "panicked" || m.status === "refused"
                    ctx.globalAlpha = amap.focusHop > 0 && amap.focusHop !== m.n ? 0.22 : 1
                    var col = bad ? String(root.cErr) : (m.status === "undone" || m.status === "never ran") ? String(root.cText3)
                            : m.kind === "async" ? String(root.cWarn) : m.kind === "root" ? String(root.cText2) : String(root.cPrimary)
                    ctx.strokeStyle = col; ctx.fillStyle = col; ctx.lineWidth = amap.focusHop === m.n ? 3.5 : 2
                    if (ctx.setLineDash) ctx.setLineDash(m.kind === "async" || m.status === "never ran" ? [7, 5] : [])
                    ctx.beginPath(); ctx.moveTo(s0.x, s0.y); ctx.lineTo(e0.x, e0.y); ctx.stroke()
                    if (ctx.setLineDash) ctx.setLineDash([])
                    var ang = Math.atan2(e0.y - s0.y, e0.x - s0.x)
                    ctx.beginPath(); ctx.moveTo(e0.x, e0.y)
                    ctx.lineTo(e0.x - 12 * Math.cos(ang - 0.4), e0.y - 12 * Math.sin(ang - 0.4))
                    ctx.lineTo(e0.x - 12 * Math.cos(ang + 0.4), e0.y - 12 * Math.sin(ang + 0.4))
                    ctx.closePath(); ctx.fill()
                    if (bad) {
                        ctx.lineWidth = 3
                        ctx.beginPath(); ctx.moveTo(e0.x - 9, e0.y - 9); ctx.lineTo(e0.x + 9, e0.y + 9); ctx.moveTo(e0.x + 9, e0.y - 9); ctx.lineTo(e0.x - 9, e0.y + 9); ctx.stroke()
                    }
                    // the number on the line, the message beside it (on the side the line was moved to)
                    var mx = (s0.x + e0.x) / 2, my = (s0.y + e0.y) / 2
                    ctx.beginPath(); ctx.arc(mx, my, 10, 0, 2 * Math.PI); ctx.fill()
                    ctx.fillStyle = "#10130f"; ctx.font = "bold 11px sans-serif"; ctx.textAlign = "center"; ctx.textBaseline = "middle"
                    ctx.fillText(String(m.n), mx, my)
                    var label = m.name + "  (" + (m.kind === "root" ? "from the tx" : m.kind) + (m.status !== "ok" ? ", " + m.status : "") + ")"
                    ctx.font = "11px sans-serif"; ctx.textAlign = "left"
                    var lw = ctx.measureText(label).width
                    var side = (off === 0 ? 1 : 1) * (nx >= 0 ? 1 : -1)
                    var lx = side > 0 ? mx + 16 : mx - 16 - lw
                    lx = Math.max(2, Math.min(lx, amap.width - lw - 2))
                    var ly = my + (Math.abs(nx) < 0.3 ? (ny > 0 ? 16 : -16) : 0)
                    ctx.fillStyle = String(root.cBg); ctx.fillRect(lx - 3, ly - 8, lw + 6, 16)
                    ctx.fillStyle = col; ctx.fillText(label, lx, ly)
                }
                ctx.globalAlpha = 1
            }
        }
    }
    // A program's source: the highlighted ranges as excerpts, in the order they run (a dispatch, then
    // the function it calls), with a line of context around each; or the whole file.
    component CodeView: Rectangle {
        id: cv
        property string source: ""
        property var ranges: []          // [[first, last], ...], 1-based, in reading order
        property int panicLine: 0        // the line that panicked, if this run failed
        property bool whole: false
        readonly property var lines: source.split("\n")
        function hot(n) { for (var i = 0; i < ranges.length; i++) if (n >= ranges[i][0] && n <= ranges[i][1]) return true; return false }
        // [{n, text, hot}], n = 0 for a gap marker
        readonly property var rows: {
            var out = []
            if (whole || !ranges.length) { for (var i = 0; i < lines.length; i++) out.push({ n: i + 1, text: lines[i], hot: hot(i + 1) }); return out }
            for (var r = 0; r < ranges.length; r++) {
                var first = Math.max(1, ranges[r][0] - 1), last = Math.min(lines.length, ranges[r][1] + 1)
                if (r > 0) out.push({ n: 0, text: "⋮", hot: false })
                for (var k = first; k <= last; k++) out.push({ n: k, text: lines[k - 1], hot: hot(k) })
            }
            return out
        }
        color: root.cInset; radius: root.rad; border.color: root.cLine
        implicitHeight: whole ? 380 : Math.min(380, rows.length * 17 + 16)
        onRangesChanged: scrollTimer.restart()
        onWholeChanged: scrollTimer.restart()
        Timer { id: scrollTimer; interval: 50; onTriggered: if (cv.whole && cv.ranges.length) list.positionViewAtIndex(Math.max(0, cv.ranges[0][0] - 4), ListView.Beginning); else list.positionViewAtBeginning() }
        ListView {
            id: list
            anchors.fill: parent; anchors.margins: 6
            clip: true
            model: cv.rows
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                required property var modelData
                width: list.width; height: 17
                color: modelData.n > 0 && modelData.n === cv.panicLine ? "#5a1d1a" : modelData.hot ? "#2d4a1e" : "transparent"
                Row {
                    spacing: 8
                    Text { width: 30; horizontalAlignment: Text.AlignRight; text: modelData.n ? String(modelData.n) : ""; color: root.cText3; font.family: "monospace"; font.pixelSize: 11 }
                    Text { text: modelData.text + (modelData.n > 0 && modelData.n === cv.panicLine ? "      ← panics here" : ""); color: modelData.n === 0 ? root.cText3 : modelData.hot ? root.cText : root.cText2; font.family: "monospace"; font.pixelSize: 11; textFormat: Text.PlainText }
                }
            }
        }
    }
    component Field: TextField {
        Layout.fillWidth: true; Layout.minimumWidth: 80
        color: root.cText; placeholderTextColor: root.cText3; font.pixelSize: 13
        background: Rectangle { color: root.cInset; radius: root.rad; border.color: parent.activeFocus ? root.cPrimary : root.cLine }
    }

    Rectangle { anchors.fill: parent; color: root.cBg }
    TextEdit { id: clip; visible: false }

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
                    // follows the module's address until you type in it (the first snapshot arrives after load)
                    Field { id: rpcField; objectName: "rpcField"; placeholderText: "http://127.0.0.1:3040"; text: root.st.rpcUrl || "" }
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
                            text: root.chain.state === "running" ? "running in this app, on " + (root.st.rpcUrl || "")
                                  : root.chain.state === "external" ? "using the sequencer already on " + (root.st.rpcUrl || "") + " (another app's: stop that one to run this app's own)"
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
                ActorExplainer { open: true }
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
                                property bool showDetails: false
                                // the message flow and the step-through position (0: all messages)
                                readonly property var flow: job && job.result && job.result.flow ? job.result.flow : []
                                property int hop: 0
                                readonly property var hopCode: hop > 0 && hop <= flow.length ? flow[hop - 1].code || null : null
                                onHopCodeChanged: if (hopCode) root.loadSource(hopCode.file)
                                onJobChanged: if (!job) hop = 0
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
                                    LogosButton {
                                        visible: !!(stepBox.job && stepBox.job.result && stepBox.job.result.flow)
                                        text: stepBox.showDetails ? "Hide the full states" : "Show the full states"
                                        onClicked: stepBox.showDetails = !stepBox.showDetails
                                    }
                                    Repeater {
                                        model: stepBox.job && stepBox.job.result && stepBox.job.result.actors && (stepBox.showDetails || !stepBox.job.result.flow) ? stepBox.job.result.actors : []
                                        delegate: T3 {
                                            required property var modelData
                                            required property int index
                                            Layout.fillWidth: true
                                            color: modelData.changed ? root.cText : root.cText3
                                            text: (index === 0 ? "root actor  " : "mentioned   ") + "(" + root.accountLabel(modelData.accountId) + ", " + (modelData.programName || root.shortId(modelData.programId)) + ")"
                                                  + (modelData.signs ? "  signs" : "") + (modelData.note ? "  - " + modelData.note : "")
                                                  + "\n    before: " + root.bytesText(modelData.before, modelData.beforeText).replace(/\n/g, "\n            ")
                                                  + "\n    after:  " + root.bytesText(modelData.after, modelData.afterText).replace(/\n/g, "\n            ")
                                            font.family: "monospace"
                                        }
                                    }
                                    // the actors and the messages between them
                                    // the buttons first, so they stay put while the text changes
                                    RowLayout {
                                        visible: !!stepBox.flow.length
                                        Layout.fillWidth: true
                                        LogosButton { text: "◀"; enabled: stepBox.hop > 1; onClicked: stepBox.hop-- }
                                        LogosButton { text: stepBox.hop === 0 ? "Step through ▶" : "▶"; enabled: stepBox.hop < stepBox.flow.length; onClicked: stepBox.hop++ }
                                        LogosButton { text: "Show all"; enabled: stepBox.hop > 0; onClicked: stepBox.hop = 0 }
                                        T2 {
                                            Layout.fillWidth: true
                                            text: stepBox.hop === 0 ? "All " + stepBox.flow.length + " messages. Step through them to see the code that handles each one."
                                                  : "Message " + stepBox.hop + " of " + stepBox.flow.length + ": " + stepBox.flow[stepBox.hop - 1].name
                                            color: root.cText
                                        }
                                    }
                                    T2 {
                                        visible: stepBox.hop > 0 && !!stepBox.hopCode
                                        Layout.fillWidth: true
                                        color: root.cText
                                        text: stepBox.hopCode ? stepBox.hopCode.says : ""
                                    }
                                    GridLayout {
                                        id: mapCode
                                        visible: !!stepBox.flow.length
                                        Layout.fillWidth: true
                                        // side by side when there's room for both
                                        readonly property bool wide: width >= 1250
                                        columns: stepBox.hop > 0 && wide ? 2 : 1
                                        columnSpacing: root.sp; rowSpacing: root.spS
                                        ActorMap {
                                            Layout.fillWidth: true
                                            Layout.preferredWidth: 1
                                            Layout.preferredHeight: implicitHeight
                                            Layout.alignment: Qt.AlignTop
                                            flow: stepBox.flow
                                            actors: stepBox.job && stepBox.job.result && stepBox.job.result.actors ? stepBox.job.result.actors : []
                                            jobState: stepBox.job ? stepBox.job.state : ""
                                            focusHop: stepBox.hop
                                        }
                                        ColumnLayout {
                                            visible: stepBox.hop > 0 && !!stepBox.hopCode
                                            Layout.fillWidth: true
                                            Layout.preferredWidth: 1
                                            Layout.alignment: Qt.AlignTop
                                            spacing: 2
                                            RowLayout {
                                                Layout.fillWidth: true
                                                T3 {
                                                    Layout.fillWidth: true
                                                    text: stepBox.hopCode ? "the code that handles it: " + stepBox.hopCode.file + ".rs  -  " + (root.sources[stepBox.hopCode.file] ? root.sources[stepBox.hopCode.file].origin : "loading...") : ""
                                                }
                                                LogosButton { text: codeView.whole ? "Just the lines that run" : "Whole file"; onClicked: codeView.whole = !codeView.whole }
                                            }
                                            CodeView {
                                                id: codeView
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: implicitHeight
                                                source: stepBox.hopCode && root.sources[stepBox.hopCode.file] ? root.sources[stepBox.hopCode.file].source : ""
                                                ranges: stepBox.hopCode ? stepBox.hopCode.ranges : []
                                                panicLine: stepBox.hopCode && stepBox.hopCode.panicLine && stepBox.flow[stepBox.hop - 1].status === "panicked" ? stepBox.hopCode.panicLine : 0
                                            }
                                        }
                                    }
                                    T3 {
                                        visible: !!(stepBox.job && stepBox.job.result && stepBox.job.result.flow)
                                        Layout.fillWidth: true
                                        text: "Each box is an actor: one account (column) under one program (row), with its state. Arrows are messages, numbered in the order they ran, all inside one transaction. Solid: sync (runs right away). Dashed: async (runs after the sync work). Red ✕: where it panicked or the engine refused; grey: undone or never ran, because the transaction reverted."
                                    }
                                    // the conversation, hop by hop, as the program recorded it
                                    T2 {
                                        visible: !!(stepBox.job && stepBox.job.result && stepBox.job.result.hops && stepBox.job.result.hops.length)
                                        text: "The conversation, read back from the chain (each hop wrote one line into its own actor's state; -> sent, <- received):"
                                        color: root.cText
                                    }
                                    Repeater {
                                        model: stepBox.job && stepBox.job.result && stepBox.job.result.hops ? stepBox.job.result.hops : []
                                        delegate: RowLayout {
                                            required property var modelData
                                            required property int index
                                            readonly property bool sameTx: !!modelData.sameTx
                                            Layout.fillWidth: true
                                            Rectangle {
                                                Layout.preferredWidth: 22; Layout.preferredHeight: 22; radius: 11
                                                Layout.alignment: Qt.AlignTop
                                                color: parent.sameTx ? root.cPrimary : root.cWarn
                                                Text { anchors.centerIn: parent; text: String(index + 1); color: "#10130f"; font.pixelSize: 12; font.weight: Font.DemiBold }
                                            }
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 0
                                                T3 { Layout.fillWidth: true; color: root.cText; text: modelData.text; font.family: "monospace" }
                                                T3 {
                                                    Layout.fillWidth: true
                                                    text: "on (" + root.accountLabel(modelData.accountId) + ", actor_conversation)  -  "
                                                          + (parent.parent.sameTx ? "this transaction" + (modelData.block ? ", block " + modelData.block : "")
                                                                                  : "a LATER transaction" + (modelData.block ? ", seen by block " + modelData.block : ""))
                                                    color: parent.parent.sameTx ? root.cText3 : root.cWarn
                                                }
                                            }
                                        }
                                    }
                                    T3 {
                                        visible: !!(stepBox.job && stepBox.job.result && stepBox.job.result.hops && stepBox.job.result.hops.length === 0 && stepBox.job.state !== "reverted")
                                        Layout.fillWidth: true
                                        text: "No new lines on the actors this step mentions."
                                    }
                                    T3 {
                                        visible: !!(stepBox.job && stepBox.job.state === "reverted" && stepBox.job.request.example === "actor_conversation")
                                        Layout.fillWidth: true
                                        text: stepBox.job && stepBox.job.request.step === "ping-undeclared"
                                              ? "No hops left on chain: the engine refused the whole transaction, so even account 1's \"-> sent Pong\" line was undone."
                                              : "No hops left on chain: every hop was undone, including the request the first hop recorded before the third one panicked."
                                    }
                                    T3 { visible: !!(stepBox.job && stepBox.job.result && stepBox.job.result.messageHex); Layout.fillWidth: true; text: stepBox.job && stepBox.job.result ? "message (borsh) 0x" + stepBox.job.result.messageHex : ""; font.family: "monospace"; wrapMode: Text.WrapAnywhere }
                                    T3 { visible: !!(stepBox.job && stepBox.job.txHash); Layout.fillWidth: true; text: stepBox.job ? "tx " + stepBox.job.txHash : ""; font.family: "monospace" }
                                    RowLayout {
                                        visible: !!(stepBox.job && stepBox.job.result && stepBox.job.result.actors)
                                        LogosButton {
                                            text: "See the account in Explorer"
                                            onClicked: { root.act("setSelected", [stepBox.job.result.actors[stepBox.job.result.actors.length - 1].accountId], ""); tabs.currentIndex = 1 }
                                        }
                                        T3 { text: "every actor state on it, with what each means" }
                                    }
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
                            // two lines: the account and its balance, then its actions (one line
                            // didn't fit the 520 px column: the id and balance overlapped)
                            delegate: Rectangle {
                                required property var modelData
                                Layout.fillWidth: true
                                implicitHeight: acol.implicitHeight + 10
                                radius: root.rad
                                color: modelData.accountId === root.st.selected ? root.cInset : "transparent"
                                ColumnLayout {
                                    id: acol
                                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 5
                                    spacing: 4
                                    RowLayout {
                                        Layout.fillWidth: true
                                        T2 {
                                            text: (modelData.accountId === root.st.payer ? "★ " : "  ") + (modelData.accountId === root.st.selected ? "▶ " : "  ") + root.shortId(modelData.accountId)
                                                  + (root.accountLabel(modelData.accountId) !== root.shortId(modelData.accountId) ? "  (" + root.accountLabel(modelData.accountId) + ")" : "")
                                            font.family: "monospace"; color: root.cText
                                            Layout.fillWidth: true; elide: Text.ElideRight; wrapMode: Text.NoWrap
                                        }
                                        T2 { text: modelData.onChain ? root.units(modelData.balance) : "off-chain"; color: modelData.onChain ? root.cText : root.cText3; wrapMode: Text.NoWrap }
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        LogosButton { text: "Show"; onClicked: root.act("setSelected", [modelData.accountId], "") }
                                        LogosButton { text: "Copy"; onClicked: root.copyText(modelData.accountId) }
                                        LogosButton { text: "Send to"; enabled: modelData.accountId !== tFrom.text.trim(); onClicked: { tTo.text = modelData.accountId; root.toast("Recipient set in Native transfer", false) } }
                                        LogosButton { text: "Pay with"; enabled: modelData.accountId !== root.st.payer; onClicked: root.act("setPayer", [modelData.accountId], "Payer set") }
                                        Item { Layout.fillWidth: true }
                                    }
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
                        RowLayout {
                            visible: !!root.detail
                            Layout.fillWidth: true
                            Mono { text: root.detail ? root.detail.accountId : "" }
                            LogosButton { text: "Copy"; onClicked: root.copyText(root.detail.accountId) }
                            LogosButton { text: "Send to"; onClicked: { tTo.text = root.detail.accountId; root.toast("Recipient set in Native transfer", false) } }
                        }
                        ActorExplainer {}
                        T2 {
                            visible: !!root.detail
                            text: root.detail ? (root.detail.onChain ? "nonce " + root.detail.nonce + "  -  balance " + root.units(root.detail.balance) + " raw units" : "not on chain yet: nothing has written to this account") : ""
                        }
                        T3 { visible: !!root.detail && root.detail.actorStates.length > 0; Layout.fillWidth: true; text: "Actor states on this account: one slot per program that keeps state here. Each is the actor (this account, that program)." }
                        T3 { visible: !!root.detail && root.detail.onChain === false; Layout.fillWidth: true; text: "No actor states yet: no program has written to this account. It appears on chain when one does (a transfer to it, or a call on it)." }
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
                                    RowLayout {
                                        Layout.fillWidth: true
                                        T2 { text: "actor (this account, " + (modelData.programName || root.shortId(modelData.programId)) + ")"; color: root.cText; Layout.fillWidth: true }
                                        T3 { text: modelData.size + " bytes" }
                                    }
                                    Mono {
                                        text: modelData.balance !== undefined ? "balance " + root.units(modelData.balance) + " raw units"
                                              : modelData.text !== undefined ? "“" + modelData.text + "”" : "0x" + modelData.hex
                                    }
                                    T3 { Layout.fillWidth: true; text: root.stateMeaning(modelData.programName || "") }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        T3 { text: "program " + modelData.programId + "  -  " + root.programKind(modelData.programId); font.family: "monospace"; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere }
                                        LogosButton { text: "Copy id"; onClicked: root.copyText(modelData.programId) }
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
                                onClicked: root.act("transfer", [JSON.stringify({ from: tFrom.text.trim(), to: tTo.text.trim(), amount: tAmount.text })], "Sent - waiting for the chain")
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
