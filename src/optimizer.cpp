//                        Caide C++ inliner
//
// This file is distributed under the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version. See LICENSE.TXT for details.

#include "optimizer.h"
#include "DependenciesCollector.h"
#include "MergeNamespacesVisitor.h"
#include "OptimizerVisitor.h"
#include "RemoveInactivePreprocessorBlocks.h"
#include "SmartRewriter.h"
#include "SourceInfo.h"
#include "util.h"

//#define CAIDE_DEBUG_MODE
#include "caide_debug.h"


#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Sema/Sema.h>
#include <clang/Tooling/Tooling.h>


#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>


using namespace clang;
using std::set;
using std::string;
using std::vector;


namespace caide {
namespace internal {

// The 'optimizer' stage acts on a single source file without dependencies (except for system headers).
// It removes code unreachable from main function.
//
// In the following, it is important to distinguish 'semantic' and 'lexical' declarations.
//
// A semantic declaration is what a user (programmer) thinks of: *the* function f(), *the* class A.
// Note that different instantiations (implicit or explicit) of the same template are different
// semantic declarations.
//
// A lexical declaration is a node in the AST (represented by clang::Decl class) coming from a
// specific place in source code. Because of implicit code and template instantiations, multiple
// declarations may be generated by the same place in the source code.
//
// A semantic declaration may have multiple corresponding lexical declarations. For example,
// a class may have multiple forward declarations and one definition. We represent a semantic
// declarations by singling out one corresponding lexical declaration, given by
// Decl::getCanonicalDecl() method.
//
// Implementation is roughly as follows:
//
// 1. Build dependency graph for semantic declarations (defined either in main file or in system
//    headers).
// 2. Find semantic declarations that are reachable from main function in the graph.
// 3. Remove unnecessary lexical declarations from main file. If a semantic declaration is unused,
//    all corresponding lexical declarations may be removed. Otherwise, a deeper analysis, depending
//    on the type of the declaration, is required. For example, a forward declaration of a used class
//    might be removed.
// 4. Remove inactive preprocessor branches that have not yet been removed.
// 5. Remove preprocessor definitions, all usages of which are inside removed code.
//
//

class OptimizerConsumer: public ASTConsumer {
public:
    OptimizerConsumer(CompilerInstance& compiler_, std::unique_ptr<SmartRewriter> smartRewriter_,
                RemoveInactivePreprocessorBlocks& ppCallbacks_,
                string& result_)
        : compiler(compiler_)
        , sourceManager(compiler.getSourceManager())
        , smartRewriter(std::move(smartRewriter_))
        , ppCallbacks(ppCallbacks_)
        , result(result_)
    {}

    virtual void HandleTranslationUnit(ASTContext& Ctx) override {
#ifdef CAIDE_DEBUG_MODE
        Ctx.getTranslationUnitDecl()->dump();
#endif

        // 1. Build dependency graph for semantic declarations.
        {
            DependenciesCollector depsVisitor(sourceManager, srcInfo);
            depsVisitor.TraverseDecl(Ctx.getTranslationUnitDecl());

            // Source range of delayed-parsed template functions includes only declaration part.
            //     Force their parsing to get correct source ranges.
            //     Suppress error messages temporarily (it's OK for these functions
            //     to be malformed).
            clang::Sema& sema = compiler.getSema();
            sema.getDiagnostics().setSuppressAllDiagnostics(true);
            for (FunctionDecl* f : srcInfo.delayedParsedFunctions) {
                auto& /*ptr to clang::LateParsedTemplate*/ lpt = sema.LateParsedTemplateMap[f];
                sema.LateTemplateParser(sema.OpaqueParser, *lpt);
            }
            sema.getDiagnostics().setSuppressAllDiagnostics(false);

#ifdef CAIDE_DEBUG_MODE
            std::ofstream file("caide-graph.dot");
            depsVisitor.printGraph(file);
#endif
        }

        // 2. Find semantic declarations that are reachable from main function in the graph.
        std::unordered_set<Decl*> used;
        {
            set<Decl*> queue;
            for (Decl* decl : srcInfo.declsToKeep)
                queue.insert(decl->getCanonicalDecl());

            while (!queue.empty()) {
                Decl* decl = *queue.begin();
                queue.erase(queue.begin());
                if (used.insert(decl).second)
                    queue.insert(srcInfo.uses[decl].begin(), srcInfo.uses[decl].end());
            }
        }

        // 3. Remove unnecessary lexical declarations.
        std::unordered_set<Decl*> removedDecls;
        {
            OptimizerVisitor visitor(sourceManager, used, removedDecls, *smartRewriter);
            visitor.TraverseDecl(Ctx.getTranslationUnitDecl());
            visitor.Finalize(Ctx);
        }
        {
            MergeNamespacesVisitor visitor(sourceManager, removedDecls, *smartRewriter);
            visitor.TraverseDecl(Ctx.getTranslationUnitDecl());
        }

        // 4. Remove inactive preprocessor branches that have not yet been removed.
        // 5. Remove preprocessor definitions, all usages of which are inside removed code.
        //
        // Callbacks have been called implicitly before this method, so we only need to call
        // Finalize() method that will actually use the information collected by callbacks
        // to remove unused preprocessor code
        ppCallbacks.Finalize();

        smartRewriter->applyChanges();

        result = getResult();
    }

private:
    string getResult() const {
        if (const RewriteBuffer* rewriteBuf =
                smartRewriter->getRewriteBufferFor(sourceManager.getMainFileID()))
            return string(rewriteBuf->begin(), rewriteBuf->end());

        // No changes
        bool invalid;
        const llvm::MemoryBuffer* buf = sourceManager.getBuffer(sourceManager.getMainFileID(), &invalid);
        if (buf && !invalid)
            return string(buf->getBufferStart(), buf->getBufferEnd());
        else
            return "Inliner error"; // something's wrong
    }

private:
    CompilerInstance& compiler;
    SourceManager& sourceManager;
    std::unique_ptr<SmartRewriter> smartRewriter;
    RemoveInactivePreprocessorBlocks& ppCallbacks;
    string& result;
    SourceInfo srcInfo;
};


class OptimizerFrontendAction : public ASTFrontendAction {
private:
    string& result;
    const set<string>& macrosToKeep;
public:
    OptimizerFrontendAction(string& result_, const set<string>& macrosToKeep_)
        : result(result_)
        , macrosToKeep(macrosToKeep_)
    {}

    virtual std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance& compiler, StringRef /*file*/) override
    {
        if (!compiler.hasSourceManager())
            throw "No source manager";
        auto smartRewriter = std::unique_ptr<SmartRewriter>(
            new SmartRewriter(compiler.getSourceManager(), compiler.getLangOpts()));
        auto ppCallbacks = std::unique_ptr<RemoveInactivePreprocessorBlocks>(
            new RemoveInactivePreprocessorBlocks(compiler.getSourceManager(), *smartRewriter, macrosToKeep));
        auto consumer = std::unique_ptr<OptimizerConsumer>(
            new OptimizerConsumer(compiler, std::move(smartRewriter), *ppCallbacks, result));
        compiler.getPreprocessor().addPPCallbacks(std::move(ppCallbacks));
        return std::move(consumer);
    }
};

class OptimizerFrontendActionFactory: public tooling::FrontendActionFactory {
private:
    string& result;
    const set<string>& macrosToKeep;
public:
    OptimizerFrontendActionFactory(string& result_, const set<string>& macrosToKeep_)
        : result(result_)
        , macrosToKeep(macrosToKeep_)
    {}
    FrontendAction* create() {
        return new OptimizerFrontendAction(result, macrosToKeep);
    }
};


Optimizer::Optimizer(const vector<string>& cmdLineOptions_,
                     const vector<string>& macrosToKeep_)
    : cmdLineOptions(cmdLineOptions_)
    , macrosToKeep(macrosToKeep_.begin(), macrosToKeep_.end())
{}

string Optimizer::doOptimize(const string& cppFile) {
    std::unique_ptr<tooling::FixedCompilationDatabase> compilationDatabase(
        createCompilationDatabaseFromCommandLine(cmdLineOptions));

    vector<string> sources;
    sources.push_back(cppFile);

    clang::tooling::ClangTool tool(*compilationDatabase, sources);

    string result;
    OptimizerFrontendActionFactory factory(result, macrosToKeep);

    int ret = tool.run(&factory);
    if (ret != 0)
        throw std::runtime_error("Compilation error");

    return result;
}

}
}

