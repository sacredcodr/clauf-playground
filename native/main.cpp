#include "runner.hpp"
#include "syntax.hpp"
#include <windows.h>
#include <richedit.h>
#include <richole.h>
#include <tom.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <future>
#include <vector>
#include <regex>

namespace
{
constexpr int kRun = 101, kBytecode = 102, kAst = 103, kOpen = 104, kSave = 105, kExamples = 106;
constexpr int kLocation = 107, kSaveAs = 108;
const COLORREF kBackground = RGB(20, 22, 26), kPanel = RGB(28, 31, 37), kText = RGB(224, 228, 234);
struct App
{
    std::filesystem::path root;
    HWND window{}, editor{}, output{}, status{}, examples{};
    std::vector<HWND> buttons;
    HFONT font{}, codeFont{}, outputFont{};
    HBRUSH background = CreateSolidBrush(kBackground);
    std::future<RunResult> pending;
    bool running = false;
    std::wstring submitted;
    std::wstring resultLabel = L"Result";
    bool hasResult = false;
    HWND location{};
    int diagnosticLine = 0;
    std::filesystem::path currentFile;
    std::wstring savedSource;
    std::wstring completedStatus;
    bool formatting = false;
};
App app;
const wchar_t* exampleNames[] = {L"First run", L"Integer division", L"Division by zero", L"Assertion", L"Following an error"};
const char* exampleFiles[] = {"first-run.c", "integer-division.c", "division-by-zero.c", "assertion.c", "follow-error.c"};

std::wstring getText(HWND control)
{
    int length = GetWindowTextLengthW(control);
    std::wstring text(length + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(length);
    return text;
}

void showError(const std::exception& error)
{
    MessageBoxW(app.window, widen(error.what()).c_str(), L"Playground", MB_OK | MB_ICONERROR);
}

void highlightSource()
{
    std::wstring source(32769, L'\0');
    GETTEXTEX request{static_cast<DWORD>(source.size() * sizeof(wchar_t)), GT_DEFAULT, 1200, nullptr, nullptr};
    source.resize(static_cast<size_t>(SendMessageW(app.editor, EM_GETTEXTEX,
        reinterpret_cast<WPARAM>(&request), reinterpret_cast<LPARAM>(source.data()))));
    const auto tokens = scanSyntax(source);

    IRichEditOle* richEdit = nullptr;
    SendMessageW(app.editor, EM_GETOLEINTERFACE, 0, reinterpret_cast<LPARAM>(&richEdit));
    if (!richEdit) { return; }
    ITextDocument* document = nullptr;
    const HRESULT queried = richEdit->QueryInterface(__uuidof(ITextDocument), reinterpret_cast<void**>(&document));
    richEdit->Release();
    if (FAILED(queried)) { return; }

    // Formatting must not become an undo step or move the user's selection.
    app.formatting = true;
    document->Undo(tomSuspend, nullptr);
    long freezeCount = 0;
    document->Freeze(&freezeCount);
    auto colourRange = [&](long start, long end, COLORREF colour)
    {
        ITextRange* range = nullptr;
        if (SUCCEEDED(document->Range(start, end, &range)))
        {
            ITextFont* font = nullptr;
            if (SUCCEEDED(range->GetFont(&font)))
            {
                font->SetForeColor(static_cast<long>(colour));
                font->Release();
            }
            range->Release();
        }
    };
    colourRange(0, static_cast<long>(source.size()), kText);
    for (const auto& token : tokens)
    {
        COLORREF colour = kText;
        switch (token.kind)
        {
        case TokenKind::eKeyword: colour = RGB(194, 164, 231); break;
        case TokenKind::eNumber: colour = RGB(229, 181, 133); break;
        case TokenKind::eString: colour = RGB(170, 203, 151); break;
        case TokenKind::eComment: colour = RGB(137, 151, 160); break;
        case TokenKind::eFunction: colour = RGB(141, 192, 219); break;
        case TokenKind::eDirective: colour = RGB(216, 159, 168); break;
        }
        colourRange(token.start, token.end, colour);
    }
    document->Unfreeze(&freezeCount);
    document->Undo(tomResume, nullptr);
    document->Release();
    app.formatting = false;
    InvalidateRect(app.window, nullptr, FALSE);
}

void saveSession()
{
    std::filesystem::create_directories(app.root / ".state");
    writeUtf8(app.root / ".state/last.c", narrow(getText(app.editor)));
}

bool confirmReplace()
{
    return getText(app.editor) == app.savedSource ||
        MessageBoxW(app.window, L"Replace the current source? Changes have not been saved to a file.",
                    L"Unsaved changes", MB_OKCANCEL | MB_ICONQUESTION) == IDOK;
}

void updateResultState()
{
    const bool current = app.hasResult && getText(app.editor) == app.submitted;
    EnableWindow(app.location, current && app.diagnosticLine > 0 && !app.running);
    if (app.hasResult)
    {
        SetWindowTextW(app.status, current ? app.completedStatus.c_str() :
            L"Source changed / Run or inspect again to update the result");
    }
}

void goToDiagnostic()
{
    if (!app.hasResult || app.running || app.diagnosticLine <= 0 || getText(app.editor) != app.submitted) { return; }
    const LRESULT start = SendMessageW(app.editor, EM_LINEINDEX, app.diagnosticLine - 1, 0);
    if (start < 0) { return; }
    const LRESULT length = SendMessageW(app.editor, EM_LINELENGTH, start, 0);
    SendMessageW(app.editor, EM_SETSEL, start, start + length);
    SendMessageW(app.editor, EM_SCROLLCARET, 0, 0);
    SetFocus(app.editor);
}

void fileDialog(bool save, bool saveAs = false)
{
    if (save && !saveAs && !app.currentFile.empty())
    {
        const auto source = getText(app.editor);
        writeUtf8(app.currentFile, narrow(source));
        app.savedSource = source;
        SetWindowTextW(app.status, L"Saved to file");
        return;
    }
    if (!save && !confirmReplace()) { return; }
    wchar_t path[32768] = L"main.c";
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = app.window;
    dialog.lpstrFilter = L"C source (*.c)\0*.c\0All files\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = 32768;
    dialog.lpstrDefExt = L"c";
    dialog.Flags = OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog))) { return; }
    if (save) { writeUtf8(path, narrow(getText(app.editor))); }
    else { SetWindowTextW(app.editor, widen(readUtf8(path)).c_str()); }
    app.currentFile = path;
    app.savedSource = getText(app.editor);
    SetWindowTextW(app.status, (std::wstring(save ? L"Saved / " : L"Opened / ") + app.currentFile.filename().wstring()).c_str());
}

void startRun(int command)
{
    if (app.running) { return; }
    saveSession();
    app.submitted = getText(app.editor);
    auto source = narrow(app.submitted);
    RunMode mode = command == kAst ? RunMode::eAst : command == kBytecode ? RunMode::eBytecode : RunMode::eRun;
    app.running = true;
    app.hasResult = false;
    app.diagnosticLine = 0;
    EnableWindow(app.location, FALSE);
    SetWindowTextW(app.location, L"No source location");
    app.resultLabel = command == kAst ? L"Abstract syntax tree" : command == kBytecode ? L"Lauf bytecode" : L"Program output";
    SetWindowTextW(app.output, L"Working...");
    InvalidateRect(app.window, nullptr, TRUE);
    for (size_t index = 0; index < 3; ++index) { EnableWindow(app.buttons[index], FALSE); }
    SetWindowTextW(app.status, L"Working through clauf / Ubuntu WSL…");
    app.pending = std::async(std::launch::async, [source, mode] { return runSource(app.root, source, mode); });
    SetTimer(app.window, 1, 50, nullptr);
}

void layout()
{
    RECT rect{};
    GetClientRect(app.window, &rect);
    int width = rect.right, height = rect.bottom;
    int split = width * 56 / 100;
    MoveWindow(app.examples, 24, 24, 156, 34, TRUE);
    MoveWindow(app.buttons[3], 192, 24, 76, 34, TRUE);
    MoveWindow(app.buttons[4], 276, 24, 86, 34, TRUE);
    MoveWindow(app.buttons[0], width - 116, 24, 92, 34, TRUE);
    MoveWindow(app.buttons[1], width - 188, 83, 92, 30, TRUE);
    MoveWindow(app.buttons[2], width - 88, 83, 64, 30, TRUE);
    MoveWindow(app.editor, 64, 112, split - 76, height - 172, TRUE);
    MoveWindow(app.output, split + 12, 112, width - split - 36, height - 172, TRUE);
    for (HWND control : {app.editor, app.output})
    {
        RECT content{};
        GetClientRect(control, &content);
        content.left += control == app.editor ? 8 : 18;
        content.top += 16;
        content.right -= 18;
        content.bottom -= 16;
        SendMessageW(control, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&content));
    }
    MoveWindow(app.status, 24, height - 36, split - 36, 24, TRUE);
    MoveWindow(app.location, split + 12, height - 44, 190, 32, TRUE);
    InvalidateRect(app.window, nullptr, TRUE);
}

HWND createControl(const wchar_t* type, const wchar_t* label, DWORD style, int id)
{
    HWND control = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1,
                                   app.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
    return control;
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    try
    {
        switch (message)
        {
        case WM_CREATE:
        {
            app.window = window;
            app.font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            app.codeFont = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
            app.outputFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
            BOOL dark = TRUE;
            DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
            app.examples = createControl(L"BUTTON", L"Examples...", BS_OWNERDRAW | WS_TABSTOP, kExamples);
            const wchar_t* labels[] = {L"Run", L"Bytecode", L"AST", L"Open…", L"Save"};
            for (int index = 0; index < 5; ++index) { app.buttons.push_back(createControl(L"BUTTON", labels[index], BS_OWNERDRAW | WS_TABSTOP, kRun + index)); }
            app.editor = createControl(MSFTEDIT_CLASS, L"", ES_MULTILINE | ES_WANTRETURN | WS_VSCROLL | WS_HSCROLL | ES_AUTOHSCROLL | WS_TABSTOP, 200);
            app.output = createControl(MSFTEDIT_CLASS, L"Run your snippet to see its output.\r\n\r\nTo inspect its structure without running it, choose AST or Bytecode above.", ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_TABSTOP, 201);
            for (HWND control : {app.editor, app.output})
            {
                SendMessageW(control, EM_SETBKGNDCOLOR, 0, kPanel);
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(control == app.editor ? app.codeFont : app.outputFont), TRUE);
                CHARFORMAT2W format{};
                format.cbSize = sizeof(format);
                format.dwMask = CFM_COLOR;
                format.crTextColor = kText;
                SendMessageW(control, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
                SendMessageW(control, EM_EXLIMITTEXT, 0, control == app.editor ? 32768 : 131072);
            }
            SendMessageW(app.editor, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SCROLL);
            app.status = createControl(L"STATIC", L"Ready   /   C with clauf   /   Ubuntu WSL", 0, 202);
            app.location = createControl(L"BUTTON", L"No source location", BS_OWNERDRAW | WS_TABSTOP, kLocation);
            EnableWindow(app.location, FALSE);
            auto session = app.root / ".state/last.c";
            SetWindowTextW(app.editor, widen(readUtf8(std::filesystem::exists(session) ? session : app.root / "examples/first-run.c")).c_str());
            layout();
            return 0;
        }
        case WM_SIZE: layout(); return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {900, 540}; return 0;
        case WM_COMMAND:
        {
            int command = LOWORD(wParam);
            if (command >= kRun && command <= kAst) { startRun(command); }
            else if (command == kOpen || command == kSave || command == kSaveAs) { fileDialog(command != kOpen, command == kSaveAs); }
            else if (command == kLocation) { goToDiagnostic(); }
            else if (command == 200 && HIWORD(wParam) == EN_CHANGE && !app.formatting)
            {
                SetTimer(window, 2, 120, nullptr);
                updateResultState();
            }
            else if (command == 200 && HIWORD(wParam) == EN_VSCROLL)
            {
                InvalidateRect(window, nullptr, FALSE);
            }
            else if (command == kExamples)
            {
                HMENU menu = CreatePopupMenu();
                for (size_t index = 0; index < std::size(exampleNames); ++index) { AppendMenuW(menu, MF_STRING, index + 1, exampleNames[index]); }
                RECT button{};
                GetWindowRect(app.examples, &button);
                int selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN, button.left, button.bottom + 4, 0, window, nullptr);
                DestroyMenu(menu);
                if (selected && confirmReplace())
                {
                    SetWindowTextW(app.editor, widen(readUtf8(app.root / "examples" / exampleFiles[selected - 1])).c_str());
                    app.currentFile.clear();
                    app.savedSource.clear();
                }
            }
            return 0;
        }
        case WM_TIMER:
            if (wParam == 2)
            {
                KillTimer(window, 2);
                highlightSource();
                return 0;
            }
            if (app.running && app.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                RunResult result = app.pending.get();
                app.running = false;
                app.hasResult = true;
                KillTimer(window, 1);
                for (size_t index = 0; index < 3; ++index) { EnableWindow(app.buttons[index], TRUE); }
                std::string output = result.error.empty() ? result.output : result.error;
                if (result.output.find("panic: division by zero") != std::string::npos) { output = "The divisor evaluated to zero. Check it before dividing.\n\n" + output; }
                if (result.output.find("panic: assert failed") != std::string::npos) { output = "The assertion's condition was false. Compare the actual value with the expected one.\n\n" + output; }
                if (output.empty()) { output = "Finished without output."; }
                if (result.truncated) { output += "\n[Output limited to 64 KB]"; }
                SetWindowTextW(app.output, widen(output).c_str());
                std::smatch location;
                if (std::regex_search(result.output, location, std::regex(R"(main\.c:([1-9][0-9]{0,5}):[0-9]+)")))
                {
                    app.diagnosticLine = std::stoi(location[1].str());
                    SetWindowTextW(app.location, (L"Go to line " + std::to_wstring(app.diagnosticLine)).c_str());
                }
                std::wstring status = !result.error.empty() ? L"Runner error" : result.timedOut ? L"Time or resource limit reached" : L"Finished • exit " + std::to_wstring(result.exitCode);
                status += L" • " + std::to_wstring(result.elapsedMs) + L" ms";
                app.completedStatus = status;
                updateResultState();
            }
            return 0;
        case WM_DRAWITEM:
        {
            auto draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            bool primary = draw->CtlID == kRun;
            COLORREF fill = primary ? RGB(40, 47, 59) : RGB(35, 40, 47);
            if (draw->itemState & ODS_SELECTED) { fill = RGB(54, 62, 77); }
            if (draw->itemState & ODS_DISABLED) { fill = RGB(35, 40, 47); }
            HBRUSH brush = CreateSolidBrush(fill);
            FillRect(draw->hDC, &draw->rcItem, brush);
            DeleteObject(brush);
            if (primary && !(draw->itemState & ODS_DISABLED))
            {
                HBRUSH border = CreateSolidBrush(RGB(103, 119, 143));
                FrameRect(draw->hDC, &draw->rcItem, border);
                DeleteObject(border);
            }
            SetBkMode(draw->hDC, TRANSPARENT);
            SetTextColor(draw->hDC, draw->itemState & ODS_DISABLED ? RGB(120, 126, 136) : kText);
            SelectObject(draw->hDC, app.font);
            auto label = getText(draw->hwndItem);
            DrawTextW(draw->hDC, label.c_str(), -1, &draw->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (draw->itemState & ODS_FOCUS) { DrawFocusRect(draw->hDC, &draw->rcItem); }
            return TRUE;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORLISTBOX:
            SetTextColor(reinterpret_cast<HDC>(wParam), kText);
            SetBkColor(reinterpret_cast<HDC>(wParam), kBackground);
            return reinterpret_cast<LRESULT>(app.background);
        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            SelectObject(dc, app.font);
            SetTextColor(dc, kText);
            SetBkMode(dc, TRANSPARENT);
            RECT rect{};
            GetClientRect(window, &rect);
            RECT gutter{24, 112, 64, rect.bottom - 60};
            HBRUSH panel = CreateSolidBrush(kPanel);
            FillRect(dc, &gutter, panel);
            DeleteObject(panel);
            SelectObject(dc, app.codeFont);
            SetTextColor(dc, RGB(112, 124, 140));
            const int firstLine = static_cast<int>(SendMessageW(app.editor, EM_GETFIRSTVISIBLELINE, 0, 0));
            const int lineCount = static_cast<int>(SendMessageW(app.editor, EM_GETLINECOUNT, 0, 0));
            for (int line = firstLine; line < lineCount; ++line)
            {
                const LRESULT index = SendMessageW(app.editor, EM_LINEINDEX, line, 0);
                POINTL point{};
                SendMessageW(app.editor, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&point), index);
                const LONG top = 112 + point.y;
                if (top + 22 > gutter.bottom) { break; }
                if (top < gutter.top) { continue; }
                RECT number{24, top, 57, top + 22};
                const auto label = std::to_wstring(line + 1);
                DrawTextW(dc, label.c_str(), -1, &number, DT_RIGHT | DT_SINGLELINE);
            }
            SelectObject(dc, app.font);
            SetTextColor(dc, RGB(145, 156, 169));
            RECT shortcut{rect.right - 224, 24, rect.right - 128, 58};
            DrawTextW(dc, L"Ctrl+Enter", -1, &shortcut, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            TextOutW(dc, 24, 90, L"Source", 6);
            RECT heading{rect.right * 56 / 100 + 12, 90, rect.right - 204, 113};
            DrawTextW(dc, app.resultLabel.c_str(), -1, &heading, DT_SINGLELINE | DT_END_ELLIPSIS);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_CLOSE:
            if (app.running) { SetWindowTextW(app.status, L"Wait for the current run to finish before closing."); return 0; }
            saveSession();
            DestroyWindow(window); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
    }
    catch (const std::exception& error) { showError(error); }
    return DefWindowProcW(window, message, wParam, lParam);
}

int selfTest()
{
    std::string report;
    int failures = 0;
    auto checkSyntax = [&](const char* name, std::wstring_view source, auto verify)
    {
        const auto tokens = scanSyntax(source);
        const bool passed = verify(tokens);
        report += std::string(passed ? "PASS " : "FAIL ") + name + "\n";
        if (!passed) { ++failures; }
    };
    checkSyntax("comments shield keywords", L"/* int return */ // void", [](const auto& tokens)
    {
        return tokens.size() == 2 && tokens[0].kind == TokenKind::eComment && tokens[1].kind == TokenKind::eComment;
    });
    checkSyntax("escaped quotes shield comment markers", LR"("say \"// int\"")", [](const auto& tokens)
    {
        return tokens.size() == 1 && tokens[0].kind == TokenKind::eString && tokens[0].end == 16;
    });
    checkSyntax("hex subtraction and signed exponent", L"0x1e-2 + 1e-2", [](const auto& tokens)
    {
        return tokens.size() == 3 && tokens[0].end == 4 && tokens[2].start == 9 && tokens[2].end == 13;
    });
    checkSyntax("multiline comment and CRLF offsets", L"/* x\r\ny */\r\nint", [](const auto& tokens)
    {
        return tokens.size() == 2 && tokens[0].end == 10 && tokens[1].start == 12 && tokens[1].kind == TokenKind::eKeyword;
    });
    checkSyntax("unfinished comment", L"int /* unfinished", [](const auto& tokens)
    {
        return tokens.size() == 2 && tokens[1].kind == TokenKind::eComment && tokens[1].end == 17;
    });
    auto check = [&](const char* name, const std::string& source, RunMode mode, auto verify)
    {
        auto result = runSource(app.root, source, mode);
        bool passed = result.error.empty() && verify(result);
        report += std::string(passed ? "PASS " : "FAIL ") + name + "\n";
        if (!passed) { ++failures; report += "exit=" + std::to_string(result.exitCode) + " " + result.error + result.output + "\n"; }
    };
    check("prints 42", readUtf8(app.root / "examples/first-run.c"), RunMode::eRun, [](auto& r) { return r.exitCode == 0 && r.output.find("sint = 42") != std::string::npos; });
    check("division diagnostic", readUtf8(app.root / "examples/division-by-zero.c"), RunMode::eRun, [](auto& r) { return r.exitCode == 1 && r.output.find("division by zero") != std::string::npos; });
    check("assertion diagnostic", readUtf8(app.root / "examples/assertion.c"), RunMode::eRun, [](auto& r) { return r.exitCode == 1 && r.output.find("assert failed") != std::string::npos; });
    check("nonzero return", "int main(){return 7;}", RunMode::eRun, [](auto& r) { return r.exitCode == 7; });
    for (int code : {124, 137, 152})
    {
        check("reserved exit remains a program return", "int main(){return " + std::to_string(code) + ";}",
              RunMode::eRun, [code](auto& r) { return r.exitCode == static_cast<unsigned long>(code) && !r.timedOut; });
    }
    check("parse error", "int main( {", RunMode::eRun, [](auto& r) { return r.exitCode != 0 && !r.output.empty(); });
    check("bytecode", "int main(){while(1){} return 0;}", RunMode::eBytecode, [](auto& r) { return r.exitCode == 0 && !r.output.empty(); });
    check("AST", "int main(){return 0;}", RunMode::eAst, [](auto& r) { return r.exitCode == 0 && !r.output.empty(); });
    check("loop limit", "int main(){while(1){} return 0;}", RunMode::eRun, [](auto& r) { return r.timedOut; });
    check("recovery", "int main(){return 0;}", RunMode::eRun, [](auto& r) { return r.exitCode == 0; });
    std::filesystem::create_directories(app.root / ".native-runs");
    writeUtf8(app.root / ".native-runs/test-results.txt", report);
    return failures ? 1 : 0;
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int show)
{
    try
    {
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, 32768);
        app.root = std::filesystem::path(executable).parent_path();
        while (!std::filesystem::exists(app.root / "examples/first-run.c"))
        {
            auto parent = app.root.parent_path();
            if (parent == app.root) { throw std::runtime_error("Keep the executable inside the playground project directory."); }
            app.root = parent;
        }
        std::wstring arguments(commandLine);
        arguments.erase(arguments.find_last_not_of(L" \t\r\n") + 1);
        if (arguments == L"--self-test") { return selfTest(); }
        SetProcessDPIAware();
        HMODULE richEdit = LoadLibraryW(L"Msftedit.dll");
        if (!richEdit) { throw std::runtime_error("Windows Rich Edit is unavailable."); }
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = windowProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = app.background;
        windowClass.lpszClassName = L"SacredPlaygroundWindow";
        RegisterClassW(&windowClass);
        HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"C playground", WS_OVERLAPPEDWINDOW,
                                     CW_USEDEFAULT, CW_USEDEFAULT, 1160, 660, nullptr, nullptr, instance, nullptr);
        if (!window) { throw std::runtime_error("Could not create the application window."); }
        ShowWindow(window, show);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            if (message.message == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0)
            {
                if (message.wParam == VK_TAB)
                {
                    SetFocus(GetNextDlgTabItem(window, GetFocus(), GetKeyState(VK_SHIFT) < 0));
                    continue;
                }
                int command = message.wParam == VK_RETURN ? kRun : message.wParam == 'S' ? kSave : message.wParam == 'O' ? kOpen : 0;
                if (command == kSave && GetKeyState(VK_SHIFT) < 0) { command = kSaveAs; }
                if (command) { SendMessageW(window, WM_COMMAND, command, 0); continue; }
            }
            if (message.hwnd == app.editor || !IsDialogMessageW(window, &message))
            { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        DeleteObject(app.font);
        DeleteObject(app.codeFont);
        DeleteObject(app.outputFont);
        DeleteObject(app.background);
        FreeLibrary(richEdit);
        return 0;
    }
    catch (const std::exception& error) { showError(error); return 1; }
}
