// ============================================================================
//  Scourge VS Code 扩展 — 入口
// ----------------------------------------------------------------------------
//  职责边界：sg 可执行文件探测、命令注册（run/build/check/repl）、
//  诊断调度（保存/输入防抖 + 并发代际丢弃）、快速修复 provider。
//  check --json 的解析在 sgcheck.js，DAP 在 sgdebug.js。
// ============================================================================
'use strict';

const path = require('path');
const fs = require('fs');
const vscode = require('vscode');
const sgcheck = require('./sgcheck.js');

const SG_CHANNEL = vscode.window.createOutputChannel('Scourge');
const diagCollection = vscode.languages.createDiagnosticCollection('scourge');

// 每文档的检查代际：编辑产生新请求时递增，旧结果回来直接丢弃（防乱序覆盖）
const checkGen = new Map();
// 每文档的防抖定时器
const checkTimer = new Map();

// ---------------------------------------------------------------------------
// sg 可执行文件探测：设置 → 工作区 build-rel/build → PATH
// 命名契约：32f3374 起编译器交付名是 scg.exe（sg 与 miniconda 撞 PATH），
// 因此 scg 恒优先于 sg（sg 仅作旧构建残留的回退）。
// 返回 null 时命令与诊断都会提示用户设置 scourge.executablePath
// ---------------------------------------------------------------------------
function resolveSgPath() {
    const cfg = vscode.workspace.getConfiguration('scourge');
    const configured = (cfg.get('executablePath') || '').trim();
    if (configured) {
        if (fs.existsSync(configured)) return configured;
        return null; // 显式配置必须是事实源，错了就明说而不是悄悄换实现
    }
    for (const ws of vscode.workspace.workspaceFolders || []) {
        const candidates = [
            path.join(ws.uri.fsPath, 'build-rel', 'scg.exe'),
            path.join(ws.uri.fsPath, 'build-rel', 'sg.exe'),
            path.join(ws.uri.fsPath, 'build', 'scg.exe'),
            path.join(ws.uri.fsPath, 'build', 'scourge.exe')
        ];
        for (const c of candidates) if (fs.existsSync(c)) return c;
    }
    const extRoot = path.dirname(__dirname); // 扩展若装在仓库内，再向上找一层
    const near = [
        path.join(extRoot, 'build-rel', 'scg.exe'),
        path.join(path.dirname(extRoot), 'build-rel', 'scg.exe'),
        path.join(path.dirname(extRoot), 'build', 'scg.exe'),
        path.join(path.dirname(extRoot), 'build', 'scourge.exe')
    ];
    for (const c of near) if (fs.existsSync(c)) return c;
    for (const dir of (process.env.PATH || '').split(path.delimiter)) {
        if (!dir) continue;
        for (const n of ['scg.exe', 'scourge.exe', 'sg.exe']) {
            const p = path.join(dir, n);
            if (fs.existsSync(p)) return p;
        }
    }
    return null;
}

/** 报告 sg 不可用（一次输出通道 + 一次用户提示） */
function reportNoSg() {
    SG_CHANNEL.appendLine('[scourge] 未找到 sg 可执行文件；请设置 scourge.executablePath');
    vscode.window.showErrorMessage(
        '未找到 sg 可执行文件：请在设置中配置 scourge.executablePath',
        '打开设置'
    ).then((pick) => {
        if (pick === '打开设置') vscode.commands.executeCommand('workbench.action.openSettings', 'scourge.executablePath');
    });
}

// ---------------------------------------------------------------------------
// 诊断调度：代际防乱序 + 防抖
// ---------------------------------------------------------------------------
function scheduleCheck(doc, delayMs) {
    if (doc.languageId !== 'scourge' || doc.uri.scheme !== 'file') return;
    const key = doc.uri.toString();
    const oldTimer = checkTimer.get(key);
    if (oldTimer) clearTimeout(oldTimer);
    checkTimer.set(key, setTimeout(async () => {
        checkTimer.delete(key);
        await checkDocument(doc);
    }, delayMs | 0));
}

async function checkDocument(doc) {
    const sg = resolveSgPath();
    if (!sg) return; // 探测失败在命令执行处已提示，此处静默（避免输入时弹窗骚扰）
    const key = doc.uri.toString();
    const gen = (checkGen.get(key) | 0) + 1;
    checkGen.set(key, gen);
    const t0 = Date.now();
    try {
        const cfg = vscode.workspace.getConfiguration('scourge');
        const raw = await sgcheck.runCheck(sg, doc.uri.fsPath, cfg.get('checkTimeoutMs'));
        if (checkGen.get(key) !== gen) return; // 过期结果：编辑器已再次变更
        diagCollection.set(doc.uri, sgcheck.toVsCodeDiagnostics(raw, doc.uri.fsPath));
        SG_CHANNEL.appendLine(`[check] ${path.basename(doc.uri.fsPath)}: ${raw.length} 条诊断（${Date.now() - t0}ms）`);
    } catch (e) {
        if (checkGen.get(key) !== gen) return;
        SG_CHANNEL.appendLine(`[check] 失败: ${e.message}`);
    }
}

// ---------------------------------------------------------------------------
// 激活
// ---------------------------------------------------------------------------
function activate(context) {
    const runInTerminal = (title, args) => {
        const sg = resolveSgPath();
        if (!sg) { reportNoSg(); return; }
        const term = vscode.window.createTerminal({ name: title });
        term.show();
        // 终端 sendText 走用户 shell：对空格路径统一加引号
        term.sendText(`& "${sg}" ${args.join(' ')}`);
    };

    const activeSgDoc = () => {
        const ed = vscode.window.activeTextEditor;
        if (!ed || ed.document.languageId !== 'scourge') {
            vscode.window.showInformationMessage('当前没有激活的 .sg 编辑器');
            return null;
        }
        return ed.document;
    };

    const cmdRun = vscode.commands.registerCommand('scourge.run', () => {
        const doc = activeSgDoc();
        if (doc) runInTerminal(`Scourge Run: ${path.basename(doc.uri.fsPath)}`, ['run', `"${doc.uri.fsPath}"`]);
    });

    const cmdBuild = vscode.commands.registerCommand('scourge.build', () => {
        const doc = activeSgDoc();
        if (doc) runInTerminal(`Scourge Build: ${path.basename(doc.uri.fsPath)}`, ['build', `"${doc.uri.fsPath}"`]);
    });

    const cmdRepl = vscode.commands.registerCommand('scourge.repl', () => {
        runInTerminal('Scourge REPL', ['repl']);
    });

    const cmdCheck = vscode.commands.registerCommand('scourge.check', async () => {
        const doc = activeSgDoc();
        if (!doc) return;
        const genBefore = checkGen.get(doc.uri.toString()) | 0;
        await checkDocument(doc);
        const diags = diagCollection.get(doc.uri) || [];
        const stale = (checkGen.get(doc.uri.toString()) | 0) !== genBefore + 1;
        if (diags.length === 0 && !stale) {
            vscode.window.showInformationMessage(`Scourge: ${path.basename(doc.uri.fsPath)} 无错误`);
        }
    });

    // 快速修复：诊断携带的 fixits（sg 编译器已算好替换区间与文本）
    const fixProvider = vscode.languages.registerCodeActionsProvider('scourge', {
        provideCodeActions(doc, _range, ctx) {
            const actions = [];
            for (const diag of ctx.diagnostics) {
                if (!diag.sgFixits) continue;
                for (const f of diag.sgFixits) {
                    actions.push(sgcheck.fixitToAction(doc.uri, diag, f));
                }
            }
            return actions;
        }
    });

    // 事件：保存即查 / 输入防抖即查 / 关闭清诊断 / 打开首查
    const onSave = vscode.workspace.onDidSaveTextDocument((doc) => {
        if (vscode.workspace.getConfiguration('scourge').get('checkOnSave')) {
            scheduleCheck(doc, 0);
        }
    });
    const onChange = vscode.workspace.onDidChangeTextDocument((ev) => {
        if (vscode.workspace.getConfiguration('scourge').get('checkOnType')) {
            scheduleCheck(ev.document, 600);
        }
    });
    const onClose = vscode.workspace.onDidCloseTextDocument((doc) => {
        diagCollection.delete(doc.uri);
        checkGen.delete(doc.uri.toString());
    });
    const onOpen = vscode.workspace.onDidOpenTextDocument((doc) => {
        if (doc.languageId === 'scourge') scheduleCheck(doc, 300);
    });

    context.subscriptions.push(
        cmdRun, cmdBuild, cmdCheck, cmdRepl, fixProvider,
        onSave, onChange, onClose, onOpen, diagCollection, SG_CHANNEL
    );

    // 已打开的 .sg 文档补一次首查（扩展激活晚于文档打开的场景）
    for (const doc of vscode.workspace.textDocuments) {
        if (doc.languageId === 'scourge') scheduleCheck(doc, 300);
    }
}

function deactivate() {
    for (const t of checkTimer.values()) clearTimeout(t);
}

module.exports = { activate, deactivate };
