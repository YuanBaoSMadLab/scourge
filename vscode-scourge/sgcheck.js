// ============================================================================
//  Scourge VS Code 扩展 — 诊断桥（sg check --json → VS Code Diagnostics）
// ----------------------------------------------------------------------------
//  职责边界：只负责调用 sg check --json、解析其 JSON 诊断数组并转换为
//  VS Code 诊断/快速修复。不做路径探测（在 extension.js）、不做防抖调度
//  （在 extension.js）。JSON 字段是 DiagEngine 的导出契约（Diagnostic.cpp
//  JSON writer）：1 基 line/col，severity ∈ {error, warning, info}。
// ============================================================================
'use strict';

const cp = require('child_process');
const vscode = require('vscode');

/** sg 诊断 severity → VS Code DiagnosticSeverity；未知值按 error 处理（宁严勿漏） */
function toSeverity(s) {
    if (s === 'warning') return vscode.DiagnosticSeverity.Warning;
    if (s === 'info' || s === 'note') return vscode.DiagnosticSeverity.Information;
    return vscode.DiagnosticSeverity.Error;
}

/**
 * 1 基 line/col → VS Code 0 基 Position。
 * col 为 0 或缺失时退到行首（col<=1 都映射到第 0 列，避免 end<start 反转）。
 */
function toPos(line, col) {
    const l = Math.max(0, (line | 0) - 1);
    const c = Math.max(0, (col | 0) - 1);
    return new vscode.Position(l, c);
}

/** 归一化路径用于文档归属比较（Windows 大小写不敏感） */
function normPath(p) {
    return String(p || '').replace(/\//g, '\\').toLowerCase();
}

/**
 * 跑一次 sg check --json。
 * @returns {Promise<Array>} 原始诊断数组（空数组 = 全绿）
 * @throws {Error} sg 路径不可用或进程崩溃（非诊断性失败，调用方提示用户）
 */
function runCheck(sgPath, filePath, timeoutMs) {
    return new Promise((resolve, reject) => {
        let child;
        try {
            // 不走 shell：路径含空格安全；stdout 承载 JSON，stderr 只收杂音
            child = cp.spawn(sgPath, ['check', '--json', filePath], { windowsHide: true });
        } catch (e) {
            reject(new Error('无法启动 sg 进程: ' + e.message));
            return;
        }
        let out = '';
        let killed = false;
        const timer = setTimeout(() => {
            killed = true;
            try { child.kill(); } catch (_) { /* 进程已退出的清理是幂等的 */ }
        }, Math.max(2000, timeoutMs | 0 || 10000));

        child.stdout.on('data', (d) => { out += d; });
        child.on('error', (e) => {
            clearTimeout(timer);
            reject(new Error('sg 启动失败（检查 scourge.executablePath）: ' + e.message));
        });
        child.on('close', (code) => {
            clearTimeout(timer);
            if (killed) { reject(new Error('check 超时被终止')); return; }
            // exit code 无关紧要：有诊断时也是非零；只认 JSON
            // 防御：若 stdout 混入非 JSON 前缀，截取第一个 [ 到最后一个 ]
            const i = out.indexOf('[');
            const j = out.lastIndexOf(']');
            if (i < 0 || j <= i) { resolve([]); return; }
            try {
                const arr = JSON.parse(out.slice(i, j + 1));
                resolve(Array.isArray(arr) ? arr : []);
            } catch (_) {
                reject(new Error('check --json 输出解析失败（sg 版本不匹配?）'));
            }
        });
    });
}

/**
 * 原始诊断数组 → VS Code 诊断（同文件条目）。
 * notes → RelatedInformation；fixits → 诊断上的 CodeAction 数据源
 * （CodeActionProvider 读取 diagnostic.sgFixits 生成快速修复）。
 * @param {Array} raw  runCheck 的返回
 * @param {string} docPath 当前文档路径（用于过滤 + 归属）
 */
function toVsCodeDiagnostics(raw, docPath) {
    const want = normPath(docPath);
    const out = [];
    for (const d of raw) {
        if (normPath(d.file) !== want) continue; // 跨模块诊断归属各自文件
        const range = new vscode.Range(toPos(d.line, d.col), toPos(d.end_line, d.end_col));
        const diag = new vscode.Diagnostic(range, d.message, toSeverity(d.severity));
        diag.code = d.code;
        diag.source = 'scourge';
        // notes：关联位置信息（如「在此处被声明」）
        if (Array.isArray(d.notes)) {
            diag.relatedInformation = d.notes
                .filter((n) => n && n.file)
                .map((n) => new vscode.DiagnosticRelatedInformation(
                    new vscode.Location(
                        vscode.Uri.file(n.file),
                        new vscode.Range(toPos(n.line, n.col), toPos(n.line, (n.col | 0) + 1))
                    ),
                    n.message || '关联位置'
                ));
        }
        // fixits：挂在诊断上供 CodeActionProvider 消费（替换区间 + 替换文本）
        if (Array.isArray(d.fixits) && d.fixits.length > 0) {
            diag.sgFixits = d.fixits.map((f) => ({
                range: new vscode.Range(toPos(f.line, f.col), toPos(f.end_line, f.end_col)),
                replacement: f.replacement || '',
                message: f.message || '快速修复'
            }));
        }
        out.push(diag);
    }
    return out;
}

/** fixits → CodeAction（WorkspaceEdit 单文件文本替换） */
function fixitToAction(uri, diag, fixit) {
    const action = new vscode.CodeAction(fixit.message, vscode.CodeActionKind.QuickFix);
    action.edit = new vscode.WorkspaceEdit();
    action.edit.replace(uri, fixit.range, fixit.replacement);
    action.diagnostics = [diag];
    return action;
}

module.exports = { runCheck, toVsCodeDiagnostics, fixitToAction, toPos };
