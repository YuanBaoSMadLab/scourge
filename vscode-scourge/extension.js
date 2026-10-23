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
// 诊断调度：代际防乱序 + 防抖 + 全局并发闸门
// check 子进程是 ~96MB 级重进程：Trae/VS Code 恢复会话时可能一次打开
// 几十个 .sg 标签页，不封顶并发就是 16 个编译器同时起（实测进程风暴）。
// ---------------------------------------------------------------------------
const MAX_PARALLEL_CHECKS = 2;
let inFlightChecks = 0;
const checkWaiters = [];

function acquireCheckSlot() {
    if (inFlightChecks < MAX_PARALLEL_CHECKS) {
        inFlightChecks++;
        return Promise.resolve();
    }
    return new Promise((res) => checkWaiters.push(res));
}

function releaseCheckSlot() {
    inFlightChecks--;
    const next = checkWaiters.shift();
    if (next) {
        inFlightChecks++;
        next();
    }
}

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
    await acquireCheckSlot();
    // 排队期间文件可能又变了：过代际直接放弃，让最新请求接管
    if (checkGen.get(key) !== gen) { releaseCheckSlot(); return; }
    const t0 = Date.now();
    try {
        const cfg = vscode.workspace.getConfiguration('scourge');
        const raw = await sgcheck.runCheck(sg, doc.uri.fsPath, cfg.get('checkTimeoutMs'));
        if (checkGen.get(key) !== gen) return; // 过期结果：编辑器已再次变更
        diagCollection.set(doc.uri, sgcheck.toVsCodeDiagnostics(raw, doc.uri.fsPath));
        SG_CHANNEL.appendLine(`[check] ${path.basename(doc.uri.fsPath)}: ${raw.length} 条诊断（${Date.now() - t0}ms, ${path.basename(sg)}）`);
    } catch (e) {
        if (checkGen.get(key) !== gen) return;
        SG_CHANNEL.appendLine(`[check] 失败: ${e.message}`);
        // 解析失败几乎总是"PATH 上的编译器过旧，不认识新语法/新 JSON 字段"。
        // 一次性明确提示（用户点掉后本轮会话不再骚扰），而非静默怪异。
        if (/解析失败|超时/.test(e.message) && !warnedOldCompiler) {
            warnedOldCompiler = true;
            const ver = await probeCompilerVersion(sg).catch(() => '?');
            vscode.window.showWarningMessage(
                `Scourge: 诊断输出异常（编译器 ${ver}）。若仓库使用了新语法，请把 scourge.executablePath 指向仓库内 build-rel\\scg.exe，或更新安装版编译器。`,
                '打开设置'
            ).then((pick) => {
                if (pick === '打开设置') vscode.commands.executeCommand('workbench.action.openSettings', 'scourge.executablePath');
            });
        }
    } finally {
        releaseCheckSlot();
    }
}

let warnedOldCompiler = false;
let compilerVersionCache = null;
/** 取编译器版本首行（scourge x.y.z），用于异常时的用户提示 */
function probeCompilerVersion(sgPath) {
    if (compilerVersionCache) return Promise.resolve(compilerVersionCache);
    return new Promise((resolve, reject) => {
        const cp = require('child_process');
        const child = cp.spawn(sgPath, ['version'], { windowsHide: true });
        let out = '';
        child.stdout.on('data', (d) => { out += d; });
        child.on('error', reject);
        child.on('close', () => {
            const m = /scourge\s+(\S+)/.exec(out);
            const v = m ? m[1] : out.trim().slice(0, 40) || '?';
            compilerVersionCache = v;
            resolve(v);
        });
        setTimeout(() => { try { child.kill(); } catch (_) { /* 幂等 */ } reject(new Error('version 超时')); }, 5000);
    });
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
        // 引号策略按集成终端默认 shell 分支：PowerShell 需 & 调用操作符，
        // cmd/bash 下裸带引号路径即可执行（& 在那里分别是命令分隔符/后台符）。
        const quote = (p) => (p.includes(' ') ? `"${p}"` : p);
        const shell = (vscode.env.shell || '').toLowerCase();
        const invoked = shell.includes('powershell') || shell.includes('pwsh')
            ? `& ${quote(sg)}`
            : quote(sg);
        term.sendText(`${invoked} ${args.join(' ')}`);
    };

    const activeSgDoc = () => {
        const ed = vscode.window.activeTextEditor;
        if (!ed || ed.document.languageId !== 'scourge') {
            vscode.window.showInformationMessage('当前没有激活的 .sg 编辑器');
            return null;
        }
        // untitled（未保存新文件）没有磁盘路径，跑命令只会困惑——提示先保存
        if (ed.document.uri.scheme !== 'file') {
            vscode.window.showInformationMessage('请先保存文件（Ctrl+S）再运行：编译器需要磁盘上的源文件');
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

    // F5 零仪式：没有 launch.json 时直接跑当前激活的 .sg 文件；有配置时
    // 注入探测到的编译器绝对路径（DAP 子进程不再需要 sgPath 字段）。
    // 这是"跨语言者 0 学习成本"的主路径：按 F5 就该跑起来，而不是先学配置模式。
    const dbgCfgProvider = vscode.debug.registerDebugConfigurationProvider('scourge', {
        resolveDebugConfiguration(_folder, config) {
            if (!config.type && !config.request) {
                const ed = vscode.window.activeTextEditor;
                if (!ed || ed.document.languageId !== 'scourge') return null; // 交回 VS Code 引导
                config.type = 'scourge';
                config.name = 'Scourge: Run File';
                config.request = 'launch';
                config.program = ed.document.uri.fsPath;
            }
            if (!config.sgPath) {
                const sg = resolveSgPath();
                if (sg) config.sgPath = sg;
            }
            return config;
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
        // 只查当前可见的：会话恢复会把几十个文档逐个"打开"，但它们不可见
        if (doc.languageId !== 'scourge') return;
        const visible = vscode.window.visibleTextEditors.some((ed) => ed.document === doc);
        if (visible) scheduleCheck(doc, 300);
    });

    context.subscriptions.push(
        cmdRun, cmdBuild, cmdCheck, cmdRepl, fixProvider, dbgCfgProvider,
        onSave, onChange, onClose, onOpen, diagCollection, SG_CHANNEL
    );

    // 激活首查只针对当前可见编辑器：绝不满场遍历 textDocuments——
    // 会话恢复时那是几十个文档（实测 16 个编译器进程同时起）。
    for (const ed of vscode.window.visibleTextEditors) {
        if (ed.document.languageId === 'scourge') scheduleCheck(ed.document, 300);
    }

    // 可观测性：明示所用编译器版本与路径。版本漂移（如 PATH 上是旧安装版，
    // 不认识脚本模式等新语法）是假诊断的头号来源——先让它在日志里现形。
    const sg = resolveSgPath();
    if (sg) {
        probeCompilerVersion(sg).then((v) => {
            SG_CHANNEL.appendLine(`[compiler] scourge ${v} @ ${sg}`);
        }).catch(() => { /* 探测失败不影响正常使用 */ });
    }
}

function deactivate() {
    for (const t of checkTimer.values()) clearTimeout(t);
}

module.exports = { activate, deactivate };
