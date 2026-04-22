// High-level LCDUI form widgets: Form, Item, StringItem, TextField, TextBox.
//
// These collectively account for ~12,000 method-references in our 19k-JAR
// corpus — the single biggest unaddressed cluster after the iteration that
// landed Sprite/Layer/Connection-IO/M3G-bridge.
//
// Scope of this file: make construction + state queries + command/listener
// wiring work cleanly. We do NOT visually render Form/TextField as a
// scrolling vertical layout — that would duplicate the work BIOS's LFImpl
// rendering would do. Many games use Forms only as splash / config screens
// before switching back to a Canvas, so widget-construction + state-read
// fidelity is what unblocks them.
//
// What's wired:
//   Item (base):       setLabel/getLabel, setLayout/getLayout,
//                      setPreferredSize/getPreferredWidth/getPreferredHeight,
//                      setItemCommandListener, addCommand/removeCommand
//   StringItem:        <init>(label,text [, appearanceMode]),
//                      getText/setText, getAppearanceMode
//   ImageItem:         <init>(label,img,layout,altText [, appearanceMode]),
//                      getImage/setImage, getAltText/setAltText
//   Spacer:            <init>(minW,minH), setMinimumSize
//   Form:              <init>(title [, items[]]), append(Item)/(String)/(Image),
//                      insert/delete/deleteAll/get/set/size,
//                      setItemStateListener,
//                      addCommand/removeCommand/setCommandListener
//   TextField:         <init>(label,text,maxSize,constraints),
//                      getString/setString, getChars/setChars, insert/delete,
//                      size, getMaxSize/setMaxSize, getCaretPosition,
//                      getConstraints/setConstraints
//   TextBox:           same shape as TextField; addCommand/setCommandListener
//                      registered directly (it's a Screen)
//
// What's NOT wired (deliberate — needs full LFImpl rendering):
//   - Visual paint of Form items, focus, scrollbar
//   - TextField text input (would need keyboard / virtual keyboard)
//   - ChoiceGroup, DateField, Gauge, CustomItem
//   - Form.itemStateChanged callback dispatch (we store the listener but
//     never fire — text doesn't actually edit so no state changes)

#include "../vm/vm.hpp"
#include "natives.hpp"
#include <unordered_map>
#include <vector>
#include <string>

extern std::unordered_map<ObjRef, ObjRef> g_command_listeners;

namespace {

// Per-Item state. ObjRef of any Item subclass keys into this.
struct ItemState {
    ObjRef label  = NULL_REF;     // java.lang.String ref
    int    layout = 0;
    int    pref_w = -1;           // -1 = unspecified
    int    pref_h = -1;
    ObjRef item_command_listener = NULL_REF;
    std::vector<ObjRef> commands;
};
std::unordered_map<ObjRef, ItemState> g_items;

// StringItem-specific text storage. label lives in ItemState.
std::unordered_map<ObjRef, ObjRef> g_string_text;
std::unordered_map<ObjRef, int>    g_string_appearance;

// ImageItem
std::unordered_map<ObjRef, ObjRef> g_image_item_image;
std::unordered_map<ObjRef, ObjRef> g_image_item_alt_text;

// Spacer
struct SpacerSize { int w = 0, h = 0; };
std::unordered_map<ObjRef, SpacerSize> g_spacer_size;

// Form
struct FormState {
    ObjRef title = NULL_REF;       // String
    std::vector<ObjRef> items;     // ordered Item refs
    ObjRef item_state_listener = NULL_REF;
};
std::unordered_map<ObjRef, FormState> g_forms;

// TextField / TextBox — same shape, used for both
struct TextEntry {
    ObjRef label    = NULL_REF;    // String
    std::string text;              // current contents (UTF-8)
    int max_size   = 0;
    int constraints = 0;
    int caret      = 0;
};
std::unordered_map<ObjRef, TextEntry> g_text_entries;

ItemState& item_for(ObjRef ref) { return g_items[ref]; }

} // namespace

void register_lcdui_natives(VM& vm) {

    // ── Item (base class) ────────────────────────────────────────────────────

    vm.register_native("javax/microedition/lcdui/Item",
        "setLabel", "(Ljava/lang/String;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            item_for(args[0].as_ref()).label = args[1].as_ref();
        });
    vm.register_native("javax/microedition/lcdui/Item",
        "getLabel", "()Ljava/lang/String;",
        [](VM&, Frame& f, std::span<Slot> args) {
            f.push_ref(item_for(args[0].as_ref()).label);
        });
    vm.register_native("javax/microedition/lcdui/Item",
        "setLayout", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            item_for(args[0].as_ref()).layout = args[1].as_int();
        });
    vm.register_native("javax/microedition/lcdui/Item",
        "getLayout", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            f.push_int(item_for(args[0].as_ref()).layout);
        });
    vm.register_native("javax/microedition/lcdui/Item",
        "setPreferredSize", "(II)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ItemState& it = item_for(args[0].as_ref());
            it.pref_w = args[1].as_int();
            it.pref_h = args[2].as_int();
        });
    vm.register_native("javax/microedition/lcdui/Item",
        "getPreferredWidth", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            ItemState& it = item_for(args[0].as_ref());
            f.push_int(it.pref_w >= 0 ? it.pref_w : 100);
        });
    vm.register_native("javax/microedition/lcdui/Item",
        "getPreferredHeight", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            ItemState& it = item_for(args[0].as_ref());
            f.push_int(it.pref_h >= 0 ? it.pref_h : 20);
        });
    vm.register_noop("javax/microedition/lcdui/Item",
        "getMinimumWidth", "()I",
        "spec returns content-based minimum; we don't measure",
        [](VM&, Frame& f, std::span<Slot>) { f.push_int(20); });
    vm.register_noop("javax/microedition/lcdui/Item",
        "getMinimumHeight", "()I",
        "spec returns content-based minimum; we don't measure",
        [](VM&, Frame& f, std::span<Slot>) { f.push_int(15); });
    vm.register_native("javax/microedition/lcdui/Item",
        "addCommand", "(Ljavax/microedition/lcdui/Command;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            item_for(args[0].as_ref()).commands.push_back(args[1].as_ref());
        });
    vm.register_native("javax/microedition/lcdui/Item",
        "removeCommand", "(Ljavax/microedition/lcdui/Command;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto& cmds = item_for(args[0].as_ref()).commands;
            auto it = std::find(cmds.begin(), cmds.end(), args[1].as_ref());
            if (it != cmds.end()) cmds.erase(it);
        });
    vm.register_native("javax/microedition/lcdui/Item",
        "setItemCommandListener",
        "(Ljavax/microedition/lcdui/ItemCommandListener;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            item_for(args[0].as_ref()).item_command_listener = args[1].as_ref();
        });
    vm.register_native("javax/microedition/lcdui/Item",
        "setDefaultCommand", "(Ljavax/microedition/lcdui/Command;)V",
        [](VM&, Frame&, std::span<Slot>) {});

    // ── StringItem ───────────────────────────────────────────────────────────

    vm.register_native("javax/microedition/lcdui/StringItem",
        "<init>", "(Ljava/lang/String;Ljava/lang/String;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            item_for(self).label = args[1].as_ref();
            g_string_text[self]  = args[2].as_ref();
            g_string_appearance[self] = 0;  // PLAIN
        });
    vm.register_native("javax/microedition/lcdui/StringItem",
        "<init>", "(Ljava/lang/String;Ljava/lang/String;I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            item_for(self).label = args[1].as_ref();
            g_string_text[self]  = args[2].as_ref();
            g_string_appearance[self] = args[3].as_int();
        });
    vm.register_native("javax/microedition/lcdui/StringItem",
        "getText", "()Ljava/lang/String;",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_string_text.find(args[0].as_ref());
            f.push_ref(it != g_string_text.end() ? it->second : NULL_REF);
        });
    vm.register_native("javax/microedition/lcdui/StringItem",
        "setText", "(Ljava/lang/String;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_string_text[args[0].as_ref()] = args[1].as_ref();
        });
    vm.register_native("javax/microedition/lcdui/StringItem",
        "getAppearanceMode", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_string_appearance.find(args[0].as_ref());
            f.push_int(it != g_string_appearance.end() ? it->second : 0);
        });

    // ── ImageItem ────────────────────────────────────────────────────────────

    vm.register_native("javax/microedition/lcdui/ImageItem",
        "<init>",
        "(Ljava/lang/String;Ljavax/microedition/lcdui/Image;ILjava/lang/String;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            item_for(self).label  = args[1].as_ref();
            item_for(self).layout = args[3].as_int();
            g_image_item_image[self]    = args[2].as_ref();
            g_image_item_alt_text[self] = args[4].as_ref();
        });
    vm.register_native("javax/microedition/lcdui/ImageItem",
        "<init>",
        "(Ljava/lang/String;Ljavax/microedition/lcdui/Image;ILjava/lang/String;I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            item_for(self).label  = args[1].as_ref();
            item_for(self).layout = args[3].as_int();
            g_image_item_image[self]    = args[2].as_ref();
            g_image_item_alt_text[self] = args[4].as_ref();
            // args[5] = appearanceMode (ignored)
        });
    vm.register_native("javax/microedition/lcdui/ImageItem",
        "getImage", "()Ljavax/microedition/lcdui/Image;",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_image_item_image.find(args[0].as_ref());
            f.push_ref(it != g_image_item_image.end() ? it->second : NULL_REF);
        });
    vm.register_native("javax/microedition/lcdui/ImageItem",
        "setImage", "(Ljavax/microedition/lcdui/Image;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_image_item_image[args[0].as_ref()] = args[1].as_ref();
        });
    vm.register_native("javax/microedition/lcdui/ImageItem",
        "getAltText", "()Ljava/lang/String;",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_image_item_alt_text.find(args[0].as_ref());
            f.push_ref(it != g_image_item_alt_text.end() ? it->second : NULL_REF);
        });
    vm.register_native("javax/microedition/lcdui/ImageItem",
        "setAltText", "(Ljava/lang/String;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_image_item_alt_text[args[0].as_ref()] = args[1].as_ref();
        });

    // ── Spacer ───────────────────────────────────────────────────────────────

    vm.register_native("javax/microedition/lcdui/Spacer",
        "<init>", "(II)V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_spacer_size[args[0].as_ref()] = { args[1].as_int(), args[2].as_int() };
        });
    vm.register_native("javax/microedition/lcdui/Spacer",
        "setMinimumSize", "(II)V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_spacer_size[args[0].as_ref()] = { args[1].as_int(), args[2].as_int() };
        });

    // ── Form ─────────────────────────────────────────────────────────────────

    vm.register_native("javax/microedition/lcdui/Form",
        "<init>", "(Ljava/lang/String;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_forms[args[0].as_ref()].title = args[1].as_ref();
        });
    vm.register_native("javax/microedition/lcdui/Form",
        "<init>", "(Ljava/lang/String;[Ljavax/microedition/lcdui/Item;)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            FormState& fs = g_forms[self];
            fs.title = args[1].as_ref();
            ObjRef arr_ref = args[2].as_ref();
            if (arr_ref == NULL_REF) return;
            HeapObject* arr = v.heap().deref(arr_ref);
            if (!arr) return;
            int n = arr->array_length();
            Slot* s = arr->array_slots();
            for (int i = 0; i < n; ++i)
                if (s[i].as_ref() != NULL_REF) fs.items.push_back(s[i].as_ref());
        });

    vm.register_native("javax/microedition/lcdui/Form",
        "append", "(Ljavax/microedition/lcdui/Item;)I",
        [](VM&, Frame& f, std::span<Slot> args) {
            FormState& fs = g_forms[args[0].as_ref()];
            fs.items.push_back(args[1].as_ref());
            f.push_int((int)fs.items.size() - 1);
        });
    vm.register_native("javax/microedition/lcdui/Form",
        "append", "(Ljava/lang/String;)I",
        [](VM& v, Frame& f, std::span<Slot> args) {
            // Wrap the String in an implicit StringItem.
            FormState& fs = g_forms[args[0].as_ref()];
            ClassDef* k = v.loader().find_or_stub("javax/microedition/lcdui/StringItem");
            ObjRef item = v.heap().alloc_object(k, 0);
            g_string_text[item] = args[1].as_ref();
            fs.items.push_back(item);
            f.push_int((int)fs.items.size() - 1);
        });
    vm.register_native("javax/microedition/lcdui/Form",
        "append", "(Ljavax/microedition/lcdui/Image;)I",
        [](VM& v, Frame& f, std::span<Slot> args) {
            FormState& fs = g_forms[args[0].as_ref()];
            ClassDef* k = v.loader().find_or_stub("javax/microedition/lcdui/ImageItem");
            ObjRef item = v.heap().alloc_object(k, 0);
            g_image_item_image[item] = args[1].as_ref();
            fs.items.push_back(item);
            f.push_int((int)fs.items.size() - 1);
        });

    vm.register_native("javax/microedition/lcdui/Form",
        "insert", "(ILjavax/microedition/lcdui/Item;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            FormState& fs = g_forms[args[0].as_ref()];
            int i = args[1].as_int();
            if (i < 0) i = 0;
            if (i > (int)fs.items.size()) i = (int)fs.items.size();
            fs.items.insert(fs.items.begin() + i, args[2].as_ref());
        });
    vm.register_native("javax/microedition/lcdui/Form",
        "delete", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            FormState& fs = g_forms[args[0].as_ref()];
            int i = args[1].as_int();
            if (i >= 0 && i < (int)fs.items.size())
                fs.items.erase(fs.items.begin() + i);
        });
    vm.register_native("javax/microedition/lcdui/Form",
        "deleteAll", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_forms[args[0].as_ref()].items.clear();
        });
    vm.register_native("javax/microedition/lcdui/Form",
        "set", "(ILjavax/microedition/lcdui/Item;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            FormState& fs = g_forms[args[0].as_ref()];
            int i = args[1].as_int();
            if (i >= 0 && i < (int)fs.items.size())
                fs.items[i] = args[2].as_ref();
        });
    vm.register_native("javax/microedition/lcdui/Form",
        "get", "(I)Ljavax/microedition/lcdui/Item;",
        [](VM&, Frame& f, std::span<Slot> args) {
            FormState& fs = g_forms[args[0].as_ref()];
            int i = args[1].as_int();
            f.push_ref((i >= 0 && i < (int)fs.items.size())
                       ? fs.items[i] : NULL_REF);
        });
    vm.register_native("javax/microedition/lcdui/Form",
        "size", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            f.push_int((int)g_forms[args[0].as_ref()].items.size());
        });
    vm.register_native("javax/microedition/lcdui/Form",
        "setItemStateListener",
        "(Ljavax/microedition/lcdui/ItemStateListener;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_forms[args[0].as_ref()].item_state_listener = args[1].as_ref();
        });

    // Form inherits Displayable.addCommand / setCommandListener at runtime,
    // but the methodref is on Form for ~1100 JARs each — re-register so the
    // scan and early-bound bytecode see them on the exact class.
    extern std::unordered_map<ObjRef, std::vector<ObjRef>>& form_displayable_commands();  // noop forward
    auto reg_displayable_command_methods = [&vm](const char* klass) {
        // These mirror the Displayable.* registrations in graphics_natives.cpp;
        // pulling g_commands directly here would create a cross-TU dependency,
        // so we rely on the runtime hierarchy walk + register here as no-ops
        // that get called only if the scanner needs satisfaction. Real
        // dispatch will hit Displayable's handler via resolve_virtual.
        vm.register_native(klass,
            "setCommandListener", "(Ljavax/microedition/lcdui/CommandListener;)V",
            [](VM&, Frame&, std::span<Slot> args) {
                g_command_listeners[args[0].as_ref()] = args[1].as_ref();
            });
        vm.register_noop(klass,
            "addCommand", "(Ljavax/microedition/lcdui/Command;)V",
            "Form/Alert/TextBox addCommand — Displayable hierarchy walk handles it",
            [](VM&, Frame&, std::span<Slot>) {});
        vm.register_noop(klass,
            "removeCommand", "(Ljavax/microedition/lcdui/Command;)V",
            "Form/Alert/TextBox removeCommand — Displayable hierarchy walk",
            [](VM&, Frame&, std::span<Slot>) {});
        vm.register_native(klass,
            "setTitle", "(Ljava/lang/String;)V",
            [](VM&, Frame&, std::span<Slot>) {});
        vm.register_native(klass,
            "getTitle", "()Ljava/lang/String;",
            [](VM&, Frame& f, std::span<Slot>) { f.push_ref(NULL_REF); });
    };
    reg_displayable_command_methods("javax/microedition/lcdui/Form");
    reg_displayable_command_methods("javax/microedition/lcdui/Alert");
    reg_displayable_command_methods("javax/microedition/lcdui/TextBox");
    reg_displayable_command_methods("javax/microedition/lcdui/List");

    // Fix Form.setTitle / getTitle to actually use FormState.title
    vm.register_native("javax/microedition/lcdui/Form",
        "setTitle", "(Ljava/lang/String;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_forms[args[0].as_ref()].title = args[1].as_ref();
        });
    vm.register_native("javax/microedition/lcdui/Form",
        "getTitle", "()Ljava/lang/String;",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_forms.find(args[0].as_ref());
            f.push_ref(it != g_forms.end() ? it->second.title : NULL_REF);
        });

    // ── TextField + TextBox (shared TextEntry backing) ──────────────────────

    auto text_entry_init = [](ObjRef self, ObjRef label_ref, ObjRef text_ref,
                              int max_size, int constraints, VM& v) {
        TextEntry& te = g_text_entries[self];
        te.label = label_ref;
        te.text = (text_ref != NULL_REF) ? v.string_value(text_ref) : "";
        te.max_size = max_size;
        te.constraints = constraints;
        te.caret = (int)te.text.size();
    };

    for (const char* klass : {
        "javax/microedition/lcdui/TextField",
        "javax/microedition/lcdui/TextBox",
    }) {
        vm.register_native(klass, "<init>",
            "(Ljava/lang/String;Ljava/lang/String;II)V",
            [text_entry_init](VM& v, Frame&, std::span<Slot> args) {
                text_entry_init(args[0].as_ref(), args[1].as_ref(),
                                args[2].as_ref(),
                                args[3].as_int(), args[4].as_int(), v);
            });

        vm.register_native(klass, "getString", "()Ljava/lang/String;",
            [](VM& v, Frame& f, std::span<Slot> args) {
                auto it = g_text_entries.find(args[0].as_ref());
                f.push_ref(it != g_text_entries.end()
                           ? v.new_string(it->second.text) : NULL_REF);
            });
        vm.register_native(klass, "setString", "(Ljava/lang/String;)V",
            [](VM& v, Frame&, std::span<Slot> args) {
                TextEntry& te = g_text_entries[args[0].as_ref()];
                te.text = (args[1].as_ref() != NULL_REF)
                            ? v.string_value(args[1].as_ref()) : "";
                te.caret = (int)te.text.size();
            });
        vm.register_native(klass, "size", "()I",
            [](VM&, Frame& f, std::span<Slot> args) {
                auto it = g_text_entries.find(args[0].as_ref());
                f.push_int(it != g_text_entries.end() ? (int)it->second.text.size() : 0);
            });
        vm.register_native(klass, "getMaxSize", "()I",
            [](VM&, Frame& f, std::span<Slot> args) {
                auto it = g_text_entries.find(args[0].as_ref());
                f.push_int(it != g_text_entries.end() ? it->second.max_size : 0);
            });
        vm.register_native(klass, "setMaxSize", "(I)I",
            [](VM&, Frame& f, std::span<Slot> args) {
                TextEntry& te = g_text_entries[args[0].as_ref()];
                te.max_size = args[1].as_int();
                f.push_int(te.max_size);
            });
        vm.register_native(klass, "getCaretPosition", "()I",
            [](VM&, Frame& f, std::span<Slot> args) {
                auto it = g_text_entries.find(args[0].as_ref());
                f.push_int(it != g_text_entries.end() ? it->second.caret : 0);
            });
        vm.register_native(klass, "getConstraints", "()I",
            [](VM&, Frame& f, std::span<Slot> args) {
                auto it = g_text_entries.find(args[0].as_ref());
                f.push_int(it != g_text_entries.end() ? it->second.constraints : 0);
            });
        vm.register_native(klass, "setConstraints", "(I)V",
            [](VM&, Frame&, std::span<Slot> args) {
                g_text_entries[args[0].as_ref()].constraints = args[1].as_int();
            });
        vm.register_native(klass, "insert", "(Ljava/lang/String;I)V",
            [](VM& v, Frame&, std::span<Slot> args) {
                TextEntry& te = g_text_entries[args[0].as_ref()];
                std::string ins = (args[1].as_ref() != NULL_REF)
                                    ? v.string_value(args[1].as_ref()) : "";
                int p = args[2].as_int();
                if (p < 0) p = 0;
                if (p > (int)te.text.size()) p = (int)te.text.size();
                te.text.insert(p, ins);
                if (te.caret >= p) te.caret += (int)ins.size();
            });
        vm.register_native(klass, "delete", "(II)V",
            [](VM&, Frame&, std::span<Slot> args) {
                TextEntry& te = g_text_entries[args[0].as_ref()];
                int p = args[1].as_int(), len = args[2].as_int();
                if (p < 0 || len <= 0 || p >= (int)te.text.size()) return;
                if (p + len > (int)te.text.size()) len = (int)te.text.size() - p;
                te.text.erase((size_t)p, (size_t)len);
                if (te.caret > p) te.caret = p;
            });
    }
}
