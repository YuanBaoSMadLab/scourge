// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译会话 — 加载（串行 DFS）+ 检查（DAG 分层并行）
// ----------------------------------------------------------------------------
//  调度模型（设计文档第五章"增量一切"的并行基础）：
//    import 构成静态 DAG。本会话分两阶段：
//      [加载] 单线程 DFS：读盘 → 词法 → 解析 → SG-AST（顺序确定，无共享写）
//      [检查] 按 DAG 分层并行：同一层的模块互相无依赖，各自独立 TypeChecker，
//             层内用线程池并发，层间串行（后序依赖已完成，registry 只读快照）
//    层内并行不改变诊断内容；每层线程结束后由主线程按模块名字典序并入
//    各自诊断，保证 JSON / 文本通道在任意并发度下逐字节确定。
//
//  多根会话（REPL 活动模块图）：checkImported / allModulesInDependencyOrder
//  以全部已加载模块为根；单入口命令（check/interface/run）沿用单根入口。
// ============================================================================
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "scourge/Common/Diagnostic.hpp"
#include "scourge/Sema/TypeChecker.hpp"
#include "scourge/Syntax/AST.hpp"

namespace scourge {

class Session {
public:
    Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    SourceMgr& src() { return src_; }
    DiagEngine& diag() { return diag_; }

    /// 便捷路径：load + check（check / interface 命令）。
    std::shared_ptr<ModuleSymbols> compile(const std::filesystem::path& entryPath,
                                           unsigned jobs = 0);

    /// 阶段一：加载入口模块（递归 import）并解析——不检查。
    /// run 命令在两阶段之间插入缓存指纹比对（设计文档"增量一切"）：
    /// 命中即跳过检查与字节码编译，直接执行缓存内的 SG-BC。
    bool load(const std::filesystem::path& entryPath);
    /// 阶段二：对已加载模块执行全量检查（DAG 分层并行）。
    std::shared_ptr<ModuleSymbols> check(unsigned jobs = 0);

    /// 对全部已加载模块执行检查（REPL 多根会话：以活动模块图为根）。
    void checkImported(unsigned jobs = 0);

    /// 加载一个 REPL import 模块：相对当前工作目录解析 <name>.sg
    /// （含递归依赖）；已加载则幂等。诊断写入 diag()。
    bool importModule(const std::string& name);

    /// 生成指定模块的 .sgi 文本（check 成功后调用）。
    std::string emitInterface(const std::string& name);

    // ---- 字节码后端（run 命令）访问器 ----
    const ModuleRegistry& registry() const { return registry_; }
    /// 表达式类型注解表（检查时按 AST 节点缓存；check 成功后有效）。
    /// AOT MLIRGen 等后端直接查表消费类型/形状，emit 处只查不重算。
    const ExprTypeMap& exprTypes() const { return exprTypes_; }
    /// 已加载模块（依赖层序：被依赖者在前，层内字典序）——字节码编译与
    /// <top> 执行共用此序，保证全局初始化先于使用且输出确定。
    std::vector<std::string> modulesInDependencyOrder(const std::string& entry) const;
    /// 全部已加载模块的依赖层序（REPL 多根：import 模块图整体编译序）。
    std::vector<std::string> allModulesInDependencyOrder() const;
    const Module* loadedModule(const std::string& name) const;
    /// 模块源文件路径（panic 定位 file:line 用）；未加载返回空串。
    std::string loadedModulePath(const std::string& name) const;
    /// 模块 FileId（AOT 诊断定位用）；未加载返回 kInvalidFile。
    FileId loadedFileId(const std::string& name) const;
    /// 模块源文本（已换行归一化；缓存源指纹输入）。未加载返回空串。
    std::string loadedSource(const std::string& name) const;

private:
    struct Loaded {
        std::shared_ptr<Module> mod;
        std::filesystem::path path;
        FileId file = kInvalidFile;
        std::vector<std::string> deps; ///< 直接依赖模块名（去重后）
    };

    bool loadModule(const std::string& name, const std::filesystem::path& path);
    /// 依赖分层（多根）：第 i 层仅依赖 < i 层；层内模块互相独立 → 可并行。
    std::vector<std::vector<std::string>> buildLayers(
        const std::vector<std::string>& roots) const;
    std::vector<std::string> depOrder(const std::vector<std::string>& roots) const;
    void checkModule(const std::string& name);
    /// 分层并行检查核心（roots 已定）：层内工作池，层间串行，主线程定序并入。
    void checkLayers(std::vector<std::string> roots, unsigned jobs);

    SourceMgr src_;
    DiagEngine diag_;
    std::string entryStem_; ///< load() 记录的入口模块名（check 用）
    std::map<std::string, Loaded> loaded_;
    std::map<std::string, std::shared_ptr<ModuleSymbols>> checked_;
    std::vector<std::string> loadStack_;

    // ---- 并行检查共享状态（访问受 mutex_ 保护；线程各自持快照） ----
    std::mutex mutex_;
    ModuleRegistry registry_; ///< 已检查模块的只读快照集合
    /// 各模块在独立引擎上产出的诊断（线程内暂存）；层内线程全部结束后
    /// 由主线程按确定序并入主引擎 diag_，保证输出不随并发度漂移。
    std::map<std::string, std::vector<Diagnostic>> moduleDiags_;
    /// 表达式类型注解表（全部模块合并；键为 AST 节点指针，跨模块不冲突，
    /// 主线程 mutex 内定序合并 → 与并发度无关）。
    ExprTypeMap exprTypes_;
};

} // namespace scourge
