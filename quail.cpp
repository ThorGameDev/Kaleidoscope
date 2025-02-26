#include "./src/lexer.h"
#include "./src/codegen.h"
#include "./src/BinopsData.h"
#include "./src/logging.h"
#include "./src/output.h"
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

/// top ::= definition | external | expression | ';'
void JitLine() {
    resetLexer();
    std::cout << ">>> ";
    initBuffer();
    try {
        while (true) {
            switch (CurTok) {
            case tok_eof:
                std::cout << "\n";
                return;
            case tok_def:
                HandleDefinitionJit();
                break;
            case tok_extern:
                HandleExtern();
                break;
            default:
                HandleTopLevelExpression();
                break;
            }
        }
    }
    catch (CompileError ce){
        DebugLog("Error Recovered\n");
    }
}

void MainLoop(){
    InitializeBinopPrecedence();
    InitializeCodegen();
    InitializeModuleAndManagers();
    while (true){
        JitLine();
    }
}

void compileFile(char* filepath, char* savename) {
    InitializeBinopPrecedence();
    InitializeCodegen();
    InitializeModuleAndManagers();

    resetLexer();
    readFile(filepath);
    try {
        bool looping = true;
        while (looping) {
            switch (CurTok) {
            case tok_eof:
                std::cout << "\n";
                looping = false;
                break;
            case tok_def:
                HandleDefinitionFile();
                break;
            case tok_extern:
                HandleExtern();
                break;
            default:
                LogError("Invalid Top-level expression");
                break;
            }
        }
        SaveToFile(savename);
    }
    catch (CompileError ce){
        DebugLog("Failed to compile file due to errors\n");
    }
}

int main(int argc, char* argv[]) {
    // Read CLI args and set relevant data
    int argType = 0;
    std::vector<char*> filepaths;
    std::vector<char*> outputs;
    for (int i = 1; i < argc; i++){
        char* arg = argv[i];
        if (strcmp(arg, "-o") == 0){
            argType = 1;  
            continue;
        }

        if (argType == 0){
            filepaths.push_back(arg);
        } else if (argType == 1) {
            outputs.push_back(arg);
        }
    }

    // Run the main "interpreter loop" now.
    if (filepaths.size() == 0) {
        MainLoop();
    }
    if (filepaths.size() == outputs.size() && filepaths.size() > 0){
        for(int i = 0; i < filepaths.size(); i++) {
            compileFile(filepaths[i], outputs[i]);
        }
    }

    return 0;
}
