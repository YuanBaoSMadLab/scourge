// ============================================================================
//  Scourge VS Code 扩展 — 最小 DAP 调试适配器（零依赖，VS Code 内置 Node 运行）
// ----------------------------------------------------------------------------
//  职责边界：launch = `sg run <program>` 子进程托管；转发 stdout/stderr 到
//  调试控制台；panic 行解析为可点击跳转（source+line）；支持终止/重启。
//  诚实边界：语言运行时无断点/停机钩子（无 PC 级调试接口），故不宣称
//  断点能力——setBreakpoints 返回未验证空集，VS Code 断点图标会显示为
//  灰色禁用而非虚假命中。
// ============================================================================
'use strict';

const cp = require('child_process');
const fs = require('fs');
const path = require('path');

// ---------------------------------------------------------------------------
// DAP 帧协议（Content-Length 头 + JSON 体；stdin/stdout 是唯一通道，
// stderr 留给适配器自身崩溃信息便于排查）
// ---------------------------------------------------------------------------
function makeProtocol(onMessage) {
    let buf = Buffer.alloc(0);
    return {
        feed(chunk) {
            buf = Buffer.concat([buf, chunk]);
            for (;;) {
                const headEnd = buf.indexOf('\r\n\r\n');
                if (headEnd < 0) return;
                const head = buf.slice(0, headEnd).toString('utf8');
                const m = /Content-Length:\s*(\d+)/i.exec(head);
                if (!m) { buf = buf.slice(headEnd + 4); continue; }
                const len = parseInt(m[1], 10);
                if (buf.length < headEnd + 4 + len) return;
                const body = buf.slice(headEnd + 4, headEnd + 4 + len).toString('utf8');
                buf = buf.slice(headEnd + 4 + len);
                try { onMessage(JSON.parse(body)); } catch (e) {
                    process.stderr.write('[sgdebug] 消息处理失败: ' + e.message + '\n');
                }
            }
        },
        send(obj) {
            const body = Buffer.from(JSON.stringify(obj), 'utf8');
            process.stdout.write(`Content-Length: ${body.length}\r\n\r\n`);
            process.stdout.write(body);
        }
    };
}

// panic 尾行格式（双模逐字契约）：panic: <消息> at <函数> (<file>:<line>)
const PANIC_RE = /panic:\s*(.*?)\s+at\s+(\S+)\s+\((.+?):(\d+)\)/;

function session() {
    let child = null;
    let terminatedSent = false;
    let seq = 1;
    let launchArgs = null;

    const proto = makeProtocol((msg) => dispatch(msg));

    function send(command, requestSeq, body) {
        proto.send({ seq: seq++, type: 'response', request_seq: requestSeq, command, success: true, body: body || {} });
    }
    function event(name, body) {
        proto.send({ seq: seq++, type: 'event', event: name, body: body || {} });
    }
    function sendOutput(category, text, loc) {
        const body = { category: category || 'console', output: text };
        if (loc && loc.file && fs.existsSync(loc.file)) {
            body.source = { name: path.basename(loc.file), path: loc.file };
            body.line = loc.line;
            body.column = loc.col || 1;
        }
        event('output', body);
    }
    function finish(exitCode) {
        if (terminatedSent) return;
        terminatedSent = true;
        event('exited', { exitCode: exitCode | 0 });
        event('terminated');
    }
    function killChild() {
        if (child) {
            try { child.kill(); } catch (_) { /* 已退出 */ }
            child = null;
        }
    }
    function start() {
        terminatedSent = false;
        const program = path.resolve(launchArgs.program);
        // cwd 无效会让 spawn 报误导性的 ENOENT（指向 exe 而非 cwd）——先验证再传
        let cwd = launchArgs.cwd ? path.resolve(launchArgs.cwd) : path.dirname(program);
        if (!fs.existsSync(cwd)) cwd = process.cwd();
        const sgPath = launchArgs.sgPath || 'scg'; // 编译器交付名 scg（32f3374 起改名）；launch config 或 PATH
        const args = ['run', program].concat(launchArgs.args || []);
        sendOutput('console', `scourge: ${sgPath} ${args.join(' ')}\n`);
        try {
            child = cp.spawn(sgPath, args, { cwd, windowsHide: true });
        } catch (e) {
            sendOutput('stderr', `sg 启动失败: ${e.message}\n`);
            finish(1);
            return;
        }
        child.on('error', (e) => {
            sendOutput('stderr',
                `sg 启动失败（${e.code || 'UNKNOWN'}: ${e.message}）— sgPath="${sgPath}"。请在 launch.json 配置 sgPath，或在设置中配置 scourge.executablePath。\n`);
            finish(1);
        });
        child.stdout.on('data', (d) => sendOutput('stdout', d.toString()));
        child.stderr.on('data', (d) => {
            const text = d.toString();
            const m = PANIC_RE.exec(text);
            // panic 行升级为 stderr 类别并携带 source+line：调试控制台可点击跳源码
            if (m) sendOutput('stderr', text, { file: m[3], line: parseInt(m[4], 10) });
            else sendOutput('stderr', text);
        });
        child.on('close', (code) => {
            sendOutput('console', `scourge 进程退出（${code | 0}）\n`);
            finish(code | 0);
        });
    }

    function dispatch(msg) {
        if (msg.type !== 'request') return;
        switch (msg.command) {
        case 'initialize':
            send('initialize', msg.seq, {
                supportsRestartRequest: true,
                supportsTerminateRequest: true,
                supportTerminatesDebuggee: true
            });
            break;
        case 'launch':
            launchArgs = msg.arguments || {};
            send('launch', msg.seq);
            event('initialized');
            start(); // 无断点可配置：收到 initialize 即可跑（configurationDone 也兼容）
            break;
        case 'setBreakPoints':
            // 运行时无停机钩子：返回未验证空集，断点显示为禁用而非虚假命中
            send('setBreakPoints', msg.seq, { breakpoints: [] });
            break;
        case 'setBreakpoints':
            send('setBreakpoints', msg.seq, { breakpoints: [] });
            break;
        case 'threads':
            send('threads', msg.seq, { threads: [{ id: 1, name: 'main' }] });
            break;
        case 'stackTrace':
            send('stackTrace', msg.seq, { stackFrames: [], totalFrames: 0 });
            break;
        case 'configurationDone':
            send('configurationDone', msg.seq);
            break;
        case 'restart':
            send('restart', msg.seq);
            killChild();
            start();
            break;
        case 'terminate':
            send('terminate', msg.seq);
            killChild();
            finish(0);
            break;
        case 'disconnect':
            send('disconnect', msg.seq);
            killChild();
            finish(0);
            break;
        default:
            // 未知请求按不支持回应，避免会话卡死在等待响应
            proto.send({ seq: seq++, type: 'response', request_seq: msg.seq, command: msg.command, success: false, message: 'not supported' });
        }
    }

    process.stdin.on('data', (c) => proto.feed(c));
    process.stdin.resume();
}

session();
