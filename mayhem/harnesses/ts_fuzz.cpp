// Fuzz harnesses for TreeSheets. TreeSheets is a unity build (src/main.cpp) of a wxWidgets GUI app,
// so this TU includes main.cpp with the app's main() replaced by wxIMPLEMENT_APP_NO_MAIN, starts the
// real application once (TSApp::OnInit: System + a TSFrame, on a private Xvfb display), and then
// drives the app's own file entry points per input:
//   TS_MODE_LOAD    System::LoadDB          (.cts documents), then the XML and text exporters
//   TS_MODE_XML     System::Import          A_IMPXML / A_IMPXMLA (first byte picks)
//   TS_MODE_TEXT    System::Import          A_IMPTXTI/C/S/T (first byte picks)
//   TS_MODE_PASTE   Document::PasteOrDrop(wxTextDataObject) the way Edit > Paste / Paste as text /
//                   Paste TSV call it (first byte picks the action and the selected cell of a fresh
//                   3x3 doc; the rest is the clipboard text), then layout, export and undo
//   TS_MODE_SCRIPT  TreeSheetsScriptImpl::ScriptRun (what the Script menu and the agent's eval run):
//                   the input is decoded into a sequence of ts.* builtin calls on a fresh 3x3 doc
//   TS_MODE_ORACLE  a plain main() for mayhem/test.sh: load/import files, print the text export.
// State kept across inputs: the app, its frame and settings. Every document an input opens is
// deleted again (its notebook page), and unused images are purged, so inputs don't see each other.
// ts-script also resets what ScriptRun keeps on tssi (the copy_current clipboard) and the status
// bar text set_status_message writes.
#include "stdafx.h"
#include <wx/modalhook.h>
#undef wxIMPLEMENT_APP
#define wxIMPLEMENT_APP(appname) wxIMPLEMENT_APP_NO_MAIN(appname)
#include "main.cpp"

#include <wx/modalhook.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <string>

using TS = treesheets;

namespace {

// Modal dialogs (message boxes, the password prompt) would block forever without a user. This is
// wxWidgets' own hook for that: a non-wxID_NONE result is used as the dialog's result instead of
// showing it.
struct NoModalDialogs : wxModalDialogHook {
    int Enter(wxDialog *) override { return wxID_CANCEL; }
    void Exit(wxDialog *) override {}
};

// GTK needs an X display: start a private Xvfb (it dies with this process). An inherited DISPLAY is
// never used, since it may not be reachable from here. -nolock with -displayfd lets parallel
// workers pick distinct displays without the shared /tmp lock files; -noreset keeps the server from
// regenerating when its last client disconnects (that window made GTK fail to connect when many
// processes started at once). The display is only used once a real X connection to it succeeds;
// otherwise the next Xvfb is tried.
bool DisplayWorks(const std::string &disp) {
    static void *x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
    if (!x11) return true;  // no direct check possible; GTK will tell
    using Open = void *(*)(const char *);
    using Close = int (*)(void *);
    auto open = reinterpret_cast<Open>(dlsym(x11, "XOpenDisplay"));
    auto close = reinterpret_cast<Close>(dlsym(x11, "XCloseDisplay"));
    if (!open || !close) return true;
    void *d = open(disp.c_str());
    if (!d) return false;
    close(d);
    return true;
}

bool TryStartXvfb() {
    int fds[2];
    if (pipe(fds) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        close(fds[0]);
        int devnull = open("/dev/null", O_RDWR);
        dup2(devnull, 0);
        dup2(devnull, 1);
        dup2(devnull, 2);
        std::string fd = std::to_string(fds[1]);
        execlp("Xvfb", "Xvfb", "-displayfd", fd.c_str(), "-nolock", "-noreset", "-nolisten", "tcp", "-screen", "0",
               "1024x768x24", (char *)nullptr);
        _exit(127);
    }
    close(fds[1]);
    char buf[32] = {};
    ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
    close(fds[0]);
    std::string disp = n > 0 ? ":" + std::string(buf, strcspn(buf, "\n")) : "";
    if (disp.size() > 1) {
        // The server may still be finishing its setup: give it a few tries to accept a client.
        for (int i = 0; i < 20; i++) {
            if (DisplayWorks(disp)) {
                setenv("DISPLAY", disp.c_str(), 1);
                return true;
            }
            usleep(50 * 1000);
        }
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return false;
}

void StartDisplay() {
    unsetenv("DISPLAY");
    unsetenv("XAUTHORITY");
    unsetenv("WAYLAND_DISPLAY");
    for (int attempt = 0; attempt < 8; attempt++) {
        if (TryStartXvfb()) return;
        usleep(100 * 1000);
    }
    fprintf(stderr, "ts_fuzz: could not start a working Xvfb\n");
    abort();
}

// The document every input starts from (see StartApp); never closed.
TS::TSCanvas *base_canvas = nullptr;

char arg0[] = "treesheets", arg1[] = "-i", arg2[] = "-p";
char *app_argv[] = {arg0, arg1, arg2, nullptr};
int app_argc = 3;

void StartApp() {
    StartDisplay();
    setenv("GDK_BACKEND", "x11", 1);
    setenv("NO_AT_BRIDGE", "1", 1);
    if (!wxEntryStart(app_argc, app_argv)) abort();
    wxLog::EnableLogging(false);
    static NoModalDialogs hook;
    hook.Register();
    if (!wxTheApp->CallOnInit()) abort();
    // Like System::Init with no files to reopen: the app always has a document open.
    TS::sys->InitDB(10);
    base_canvas = TS::sys->frame->GetCurrentTab();
}

size_t PageCount() { return TS::sys->frame->notebook->GetPageCount(); }

// The document an input opened: the app selects each new tab.
TS::Document *NewDoc() {
    auto *canvas = TS::sys->frame->GetCurrentTab();
    return canvas && canvas != base_canvas ? canvas->doc.get() : nullptr;
}

// Close every document but the base one, and drop the images they used.
void CloseNewDocs() {
    for (size_t i = PageCount(); i-- > 0;) {
        auto *canvas = dynamic_cast<TS::TSCanvas *>(TS::sys->frame->notebook->GetPage(i));
        if (canvas == base_canvas) continue;
        if (canvas) canvas->doc->modified = false;
        TS::sys->frame->notebook->DeletePage(i);
    }
    TS::sys->frame->notebook->SetSelection(0);
    TS::sys->PurgeUnusedImages();
    // Closed tabs are only destroyed at idle time; there is no event loop, so do it here.
    wxTheApp->ProcessIdle();
}

wxString ExportText(TS::Document *doc, int action) {
    return doc->root->ToText(0, TS::Selection(), action, doc, true, doc->root.get());
}

// Writes the input to a fresh scratch file with the given suffix; returns its path.
std::string WriteTemp(const uint8_t *data, size_t size, const char *suffix) {
    const char *tmp = getenv("TMPDIR");
    std::string path = std::string(tmp && *tmp ? tmp : "/tmp") + "/ts_fuzz_XXXXXX" + suffix;
    int fd = mkstemps(path.data(), static_cast<int>(strlen(suffix)));
    if (fd < 0) abort();
    size_t off = 0;
    while (off < size) {
        ssize_t w = write(fd, data + off, size - off);
        if (w <= 0) abort();
        off += static_cast<size_t>(w);
    }
    close(fd);
    return path;
}

// Load a .cts and, when it loads, run the exporters over the loaded tree.
wxString LoadAndExport(const wxString &path, wxString *out) {
    wxString pw = "fuzz";
    // astemplate=true: skip the open-tab/autosave/file-history bookkeeping, same loader.
    wxString err = TS::sys->LoadDB(path, false, -1, &pw, true);
    auto *doc = NewDoc();
    if (err.IsEmpty() && doc) {
        wxString xml = ExportText(doc, A_EXPXML);
        wxString text = ExportText(doc, A_EXPTEXT);
        wxString html = ExportText(doc, A_EXPHTMLT);
        if (out) *out = text;
    }
    CloseNewDocs();
    return err;
}

wxString ImportAndExport(const wxString &path, int action, wxString *out) {
    wxString err = TS::sys->Import(path, action);
    auto *doc = NewDoc();
    if (err.IsEmpty() && doc) {
        wxString text = ExportText(doc, A_EXPTEXT);
        wxString xml = ExportText(doc, A_EXPXML);
        if (out) *out = text;
    }
    CloseNewDocs();
    return err;
}


#ifdef TS_MODE_PASTE
// As Document::Action does for wxID_PASTE / A_PASTETEXT / A_PASTETSV with text on the clipboard.
void PasteText(uint8_t sel, const wxString &text, wxString *out) {
    TS::sys->InitDB(3, 3);
    if (auto *doc = NewDoc()) {
        static const int actions[] = {wxID_PASTE, A_PASTETEXT, A_PASTETSV, wxID_PASTE};
        int action = actions[sel & 3];
        int x = (sel >> 2) % 3, y = (sel >> 4) % 3;
        doc->SetSelect(TS::Selection(doc->root->grid, x, y, 1, 1));
        int pastemode = doc->selected.PasteMode();
        if (doc->selected.ThinExpand(doc) != nullptr) {
            wxTextDataObject tdo(text);
            doc->PasteOrDrop(tdo, pastemode, action != wxID_PASTE, action == A_PASTETSV);
            doc->UpdateLayout();
            wxString t = ExportText(doc, A_EXPTEXT);
            if (out) *out = t;
            if (!doc->undolist.empty()) {
                doc->Undo(doc->undolist, doc->redolist);
                doc->UpdateLayout();
            }
        }
    }
    CloseNewDocs();
}
#endif

#ifdef TS_MODE_SCRIPT
// The ts.* builtins a script may call, by argument shape. Left out on purpose: the ones that open
// dialogs, read or write files, or touch the window/agent socket (get_filename_from_user,
// load_document, save_document*, set_password, set_autoexport, set_image, set_window_size,
// close_agent_socket, agent_result). Arguments: i = int, s = string, b = bool, v = int2, c = color.
struct Op { const char *name, *args; };
const Op ops[] = {
    {"goto_root", ""}, {"goto_view", ""}, {"zoom", "i"}, {"zoom_to_root", ""},
    {"has_selection", ""}, {"goto_selection", ""}, {"has_parent", ""}, {"goto_parent", ""},
    {"num_children", ""}, {"num_columns_rows", ""}, {"goto_child", "i"},
    {"goto_column_row", "ii"}, {"select", ""}, {"select_range", "vv"}, {"get_text", ""},
    {"get_note", ""}, {"get_formula", ""}, {"set_formula", "s"}, {"set_text", "s"},
    {"set_note", "s"}, {"create_grid", "ii"}, {"insert_column", "i"}, {"insert_row", "i"},
    {"delete", "vv"}, {"set_background_color", "cb"}, {"set_text_color", "cb"},
    {"set_text_filtered", "b"}, {"is_text_filtered", ""}, {"set_border_color", "cb"},
    {"get_relative_size", ""}, {"set_relative_size", "i"}, {"set_style_bits", "i"},
    {"get_style_bits", ""}, {"set_span_style_bits", "iii"}, {"get_span_style_bits", "i"},
    {"set_span_text_color", "iic"}, {"get_span_text_color", "i"}, {"set_link", "iisb"},
    {"get_link", "i"}, {"set_text_alignment", "i"}, {"get_text_alignment", ""},
    {"set_vertical_alignment", "i"}, {"get_vertical_alignment", ""}, {"get_render_style", ""},
    {"is_vertical_layout", ""}, {"set_status_message", "s"}, {"get_filename", ""},
    {"new_document", "ii"}, {"get_last_edit", ""}, {"is_tag", ""}, {"has_image", ""},
    {"get_column_width", ""}, {"set_column_width", "i"}, {"remove_image", ""},
    {"set_image_display_scale", "i"}, {"undo", ""}, {"redo", ""}, {"is_grid", ""},
    {"get_cell_type", ""}, {"is_folded", ""}, {"set_folded", "b"}, {"get_background_color", ""},
    {"get_text_color", ""}, {"get_border_color", ""}, {"find_exact", "s"}, {"copy_current", ""},
    {"paste_into_current", ""}, {"get_subtree_text", "i"}, {"set_cell_type", "i"},
    {"evaluate", "i"}, {"grid_count", ""}, {"grid_sum", ""}, {"grid_min", ""}, {"grid_max", ""},
    {"grid_avg", ""}, {"grid_median", ""},
};

struct Reader {
    const uint8_t *p, *end;
    uint8_t Byte() { return p < end ? *p++ : 0; }
    // Mostly small values (-64..63), sometimes any 32-bit value, so sizes can be negative or huge.
    long long Int() {
        uint8_t b = Byte();
        if (!(b & 0x80)) return static_cast<int8_t>(b << 1) >> 1;
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) v = v << 8 | Byte();
        return static_cast<int32_t>(v);
    }
    std::string Str() {
        // Characters a Lobster string literal takes as they are; formulas and links need = + ( ) etc.
        static const char alphabet[] = "abcxyzABC0123456789 =+-*/.,;:()[]<>#_!?%&|^~@$";
        std::string out = "\"";
        int n = Byte() % 24;
        for (int i = 0; i < n; i++) out += alphabet[Byte() % (sizeof(alphabet) - 1)];
        return out + "\"";
    }
};

std::string DecodeScript(const uint8_t *data, size_t size) {
    Reader r {data, data + size};
    std::string code = "import std\n";
    for (int line = 0; line < 64 && r.p < r.end; line++) {
        const Op &op = ops[r.Byte() % (sizeof(ops) / sizeof(ops[0]))];
        std::string call = "ts." + std::string(op.name) + "(";
        for (const char *a = op.args; *a; a++) {
            if (a != op.args) call += ", ";
            switch (*a) {
                case 'i': call += std::to_string(r.Int()); break;
                case 's': call += r.Str(); break;
                case 'b': call += (r.Byte() & 1) ? "true" : "false"; break;
                case 'v': {
                    auto x = r.Int();
                    call += "int2 { " + std::to_string(x) + ", " + std::to_string(r.Int()) + " }";
                    break;
                }
                case 'c': {
                    call += "float4 {";
                    for (int i = 0; i < 4; i++) call += (i ? ", " : " ") + std::to_string(r.Byte() / 255.0);
                    call += " }";
                    break;
                }
            }
        }
        code += call + ")\n";
    }
    return code;
}

wxString RunScript(const std::string &code, wxString *out) {
    TS::sys->InitDB(3, 3);
    std::string err = TS::tssi.ScriptRun("fuzz.lobster", code);
    if (out) {
        if (auto *doc = NewDoc()) *out = ExportText(doc, A_EXPTEXT);
    }
    CloseNewDocs();
    // ScriptRun keeps the ts.copy_current clipboard and the status text; reset both so the next
    // input starts from the same state.
    TS::tssi.script_clipboard.reset();
    TS::tssi.script_clipboard_colwidth = 0;
    if (TS::sys->frame->GetStatusBar()) TS::sys->frame->SetStatusText(wxEmptyString, 0);
    return wxString::FromUTF8(err);
}
#endif
}  // namespace

#ifndef TS_MODE_ORACLE

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
    StartApp();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
#if defined(TS_MODE_LOAD)
    std::string path = WriteTemp(data, size, ".cts");
    LoadAndExport(wxString::FromUTF8(path), nullptr);
#elif defined(TS_MODE_XML)
    if (size < 1) return -1;
    int action = (data[0] & 1) ? A_IMPXMLA : A_IMPXML;
    std::string path = WriteTemp(data + 1, size - 1, ".xml");
    ImportAndExport(wxString::FromUTF8(path), action, nullptr);
#elif defined(TS_MODE_TEXT)
    if (size < 1) return -1;
    static const int actions[] = {A_IMPTXTI, A_IMPTXTC, A_IMPTXTS, A_IMPTXTT};
    int action = actions[data[0] & 3];
    std::string path = WriteTemp(data + 1, size - 1, ".txt");
    ImportAndExport(wxString::FromUTF8(path), action, nullptr);
#elif defined(TS_MODE_PASTE)
    if (size < 1) return -1;
    PasteText(data[0], wxString::FromUTF8(reinterpret_cast<const char *>(data + 1), size - 1), nullptr);
    std::string path;
#elif defined(TS_MODE_SCRIPT)
    RunScript(DecodeScript(data, size), nullptr);
    std::string path;
#else
#error "define TS_MODE_LOAD, TS_MODE_XML, TS_MODE_TEXT, TS_MODE_PASTE or TS_MODE_SCRIPT"
#endif
    if (!path.empty()) unlink(path.c_str());
    return 0;
}

#else  // TS_MODE_ORACLE

// usage: ts_oracle load <file.cts> | xml <file.xml> | csv <file> | tsv <file> | indent <file> | script <file.lobster> | paste <file.txt>
// Prints the document's plain-text export, or "ERROR: <message>".
int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s load|xml|csv|tsv|indent <file>\n", argv[0]);
        return 2;
    }
    StartApp();
    std::string mode = argv[1];
    wxString path = wxString::FromUTF8(argv[2]), out, err;
    if (mode == "load") err = LoadAndExport(path, &out);
    else if (mode == "xml") err = ImportAndExport(path, A_IMPXML, &out);
    else if (mode == "csv") err = ImportAndExport(path, A_IMPTXTC, &out);
    else if (mode == "tsv") err = ImportAndExport(path, A_IMPTXTT, &out);
    else if (mode == "indent") err = ImportAndExport(path, A_IMPTXTI, &out);
#ifdef TS_MODE_PASTE
    else if (mode == "paste") {
        wxString text;
        wxFFile f(path);
        if (!f.IsOpened() || !f.ReadAll(&text)) return 2;
        PasteText(0, text, &out);
    }
#endif
#ifdef TS_MODE_SCRIPT
    else if (mode == "script") {
        wxString code;
        wxFFile f(path);
        if (!f.IsOpened() || !f.ReadAll(&code)) return 2;
        err = RunScript(std::string(code.utf8_str()), &out);
    }
#endif
    else return 2;
    if (!err.IsEmpty()) {
        printf("ERROR: %s\n", (const char *)err.utf8_str());
        fflush(stdout);
        _exit(1);
    }
    fputs((const char *)out.utf8_str(), stdout);
    fflush(stdout);
    _exit(0);  // skip GUI teardown
}

#endif
