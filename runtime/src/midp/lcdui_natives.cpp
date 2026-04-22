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

    // ── ChoiceGroup (also covers Choice interface methods) ───────────────────
    // EXCLUSIVE=1, MULTIPLE=2, IMPLICIT=3, POPUP=4
    struct ChoiceState {
        int type = 1;
        std::vector<ObjRef> texts;     // String refs
        std::vector<ObjRef> images;    // Image refs (parallel)
        std::vector<bool>   selected;  // parallel; for EXCLUSIVE/IMPLICIT/POPUP
                                       // exactly one is true at a time
        int fit_policy = 0;
    };
    static std::unordered_map<ObjRef, ChoiceState> g_choices;
    auto choice_for = [](ObjRef r) -> ChoiceState* {
        auto it = g_choices.find(r);
        return it == g_choices.end() ? nullptr : &it->second;
    };

    auto choice_init_arrays = [](VM& v, ObjRef self, int type,
                                 ObjRef texts_arr, ObjRef images_arr) {
        ChoiceState& cs = g_choices[self];
        cs.type = type;
        if (texts_arr != NULL_REF) {
            HeapObject* a = v.heap().deref(texts_arr);
            if (a) {
                int n = a->array_length();
                Slot* s = a->array_slots();
                for (int i = 0; i < n; ++i) cs.texts.push_back(s[i].as_ref());
            }
        }
        if (images_arr != NULL_REF) {
            HeapObject* a = v.heap().deref(images_arr);
            if (a) {
                int n = a->array_length();
                Slot* s = a->array_slots();
                for (int i = 0; i < n && i < (int)cs.texts.size(); ++i)
                    cs.images.push_back(s[i].as_ref());
            }
        }
        while (cs.images.size() < cs.texts.size()) cs.images.push_back(NULL_REF);
        cs.selected.resize(cs.texts.size(), false);
        if (!cs.selected.empty() && (type == 1 || type == 3 || type == 4))
            cs.selected[0] = true;
    };

    for (const char* k : {
        "javax/microedition/lcdui/ChoiceGroup",
        "javax/microedition/lcdui/List",  // List is also a Choice
    }) {
        vm.register_native(k, "<init>",
            "(Ljava/lang/String;I)V",
            [choice_init_arrays](VM& v, Frame&, std::span<Slot> args) {
                choice_init_arrays(v, args[0].as_ref(), args[2].as_int(),
                                   NULL_REF, NULL_REF);
                item_for(args[0].as_ref()).label = args[1].as_ref();
            });
        vm.register_native(k, "<init>",
            "(Ljava/lang/String;I[Ljava/lang/String;[Ljavax/microedition/lcdui/Image;)V",
            [choice_init_arrays](VM& v, Frame&, std::span<Slot> args) {
                choice_init_arrays(v, args[0].as_ref(), args[2].as_int(),
                                   args[3].as_ref(), args[4].as_ref());
                item_for(args[0].as_ref()).label = args[1].as_ref();
            });

        vm.register_native(k, "append",
            "(Ljava/lang/String;Ljavax/microedition/lcdui/Image;)I",
            [choice_for](VM&, Frame& f, std::span<Slot> args) {
                ChoiceState* cs = choice_for(args[0].as_ref());
                if (!cs) { f.push_int(-1); return; }
                cs->texts.push_back(args[1].as_ref());
                cs->images.push_back(args[2].as_ref());
                cs->selected.push_back(false);
                f.push_int((int)cs->texts.size() - 1);
            });
        vm.register_native(k, "insert",
            "(ILjava/lang/String;Ljavax/microedition/lcdui/Image;)V",
            [choice_for](VM&, Frame&, std::span<Slot> args) {
                ChoiceState* cs = choice_for(args[0].as_ref());
                if (!cs) return;
                int i = args[1].as_int();
                if (i < 0) i = 0;
                if (i > (int)cs->texts.size()) i = (int)cs->texts.size();
                cs->texts.insert(cs->texts.begin() + i, args[2].as_ref());
                cs->images.insert(cs->images.begin() + i, args[3].as_ref());
                cs->selected.insert(cs->selected.begin() + i, false);
            });
        vm.register_native(k, "delete", "(I)V",
            [choice_for](VM&, Frame&, std::span<Slot> args) {
                ChoiceState* cs = choice_for(args[0].as_ref());
                if (!cs) return;
                int i = args[1].as_int();
                if (i < 0 || i >= (int)cs->texts.size()) return;
                cs->texts.erase(cs->texts.begin() + i);
                cs->images.erase(cs->images.begin() + i);
                cs->selected.erase(cs->selected.begin() + i);
            });
        vm.register_native(k, "deleteAll", "()V",
            [choice_for](VM&, Frame&, std::span<Slot> args) {
                ChoiceState* cs = choice_for(args[0].as_ref());
                if (cs) { cs->texts.clear(); cs->images.clear(); cs->selected.clear(); }
            });
        vm.register_native(k, "set",
            "(ILjava/lang/String;Ljavax/microedition/lcdui/Image;)V",
            [choice_for](VM&, Frame&, std::span<Slot> args) {
                ChoiceState* cs = choice_for(args[0].as_ref());
                if (!cs) return;
                int i = args[1].as_int();
                if (i < 0 || i >= (int)cs->texts.size()) return;
                cs->texts[i]  = args[2].as_ref();
                cs->images[i] = args[3].as_ref();
            });
        vm.register_native(k, "size", "()I",
            [choice_for](VM&, Frame& f, std::span<Slot> args) {
                ChoiceState* cs = choice_for(args[0].as_ref());
                f.push_int(cs ? (int)cs->texts.size() : 0);
            });
        vm.register_native(k, "getString", "(I)Ljava/lang/String;",
            [choice_for](VM&, Frame& f, std::span<Slot> args) {
                ChoiceState* cs = choice_for(args[0].as_ref());
                int i = args[1].as_int();
                f.push_ref((cs && i >= 0 && i < (int)cs->texts.size())
                           ? cs->texts[i] : NULL_REF);
            });
        vm.register_native(k, "getImage", "(I)Ljavax/microedition/lcdui/Image;",
            [choice_for](VM&, Frame& f, std::span<Slot> args) {
                ChoiceState* cs = choice_for(args[0].as_ref());
                int i = args[1].as_int();
                f.push_ref((cs && i >= 0 && i < (int)cs->images.size())
                           ? cs->images[i] : NULL_REF);
            });
        vm.register_native(k, "isSelected", "(I)Z",
            [choice_for](VM&, Frame& f, std::span<Slot> args) {
                ChoiceState* cs = choice_for(args[0].as_ref());
                int i = args[1].as_int();
                f.push_int((cs && i >= 0 && i < (int)cs->selected.size()
                            && cs->selected[i]) ? 1 : 0);
            });
        vm.register_native(k, "setSelectedIndex", "(IZ)V",
            [choice_for](VM&, Frame&, std::span<Slot> args) {
                ChoiceState* cs = choice_for(args[0].as_ref());
                if (!cs) return;
                int i = args[1].as_int();
                bool v = args[2].as_int() != 0;
                if (i < 0 || i >= (int)cs->selected.size()) return;
                if (cs->type == 1 || cs->type == 3 || cs->type == 4) {
                    // EXCLUSIVE/IMPLICIT/POPUP: exactly one true
                    for (auto&& b : cs->selected) b = false;
                    cs->selected[i] = true;
                } else {
                    cs->selected[i] = v;
                }
            });
        vm.register_native(k, "getSelectedIndex", "()I",
            [choice_for](VM&, Frame& f, std::span<Slot> args) {
                ChoiceState* cs = choice_for(args[0].as_ref());
                if (!cs) { f.push_int(-1); return; }
                for (int i = 0; i < (int)cs->selected.size(); ++i)
                    if (cs->selected[i]) { f.push_int(i); return; }
                f.push_int(-1);
            });
        vm.register_native(k, "setSelectedFlags", "([Z)V",
            [choice_for](VM& v, Frame&, std::span<Slot> args) {
                ChoiceState* cs = choice_for(args[0].as_ref());
                if (!cs || args[1].as_ref() == NULL_REF) return;
                HeapObject* a = v.heap().deref(args[1].as_ref());
                if (!a) return;
                int n = std::min(a->array_length(), (int)cs->selected.size());
                uint8_t* src = a->array_bytes();
                for (int i = 0; i < n; ++i) cs->selected[i] = src[i] != 0;
            });
        vm.register_native(k, "getSelectedFlags", "([Z)I",
            [choice_for](VM& v, Frame& f, std::span<Slot> args) {
                ChoiceState* cs = choice_for(args[0].as_ref());
                if (!cs || args[1].as_ref() == NULL_REF) { f.push_int(0); return; }
                HeapObject* a = v.heap().deref(args[1].as_ref());
                if (!a) { f.push_int(0); return; }
                int n = std::min(a->array_length(), (int)cs->selected.size());
                int count = 0;
                uint8_t* dst = a->array_bytes();
                for (int i = 0; i < n; ++i) {
                    dst[i] = cs->selected[i] ? 1 : 0;
                    if (cs->selected[i]) count++;
                }
                f.push_int(count);
            });
        vm.register_native(k, "setFitPolicy", "(I)V",
            [choice_for](VM&, Frame&, std::span<Slot> args) {
                ChoiceState* cs = choice_for(args[0].as_ref());
                if (cs) cs->fit_policy = args[1].as_int();
            });
        vm.register_native(k, "getFitPolicy", "()I",
            [choice_for](VM&, Frame& f, std::span<Slot> args) {
                ChoiceState* cs = choice_for(args[0].as_ref());
                f.push_int(cs ? cs->fit_policy : 0);
            });
    }

    // ── Gauge ────────────────────────────────────────────────────────────────
    struct GaugeState {
        int value = 0;
        int max_value = 100;
        bool interactive = false;
    };
    static std::unordered_map<ObjRef, GaugeState> g_gauges;
    vm.register_native("javax/microedition/lcdui/Gauge",
        "<init>", "(Ljava/lang/String;ZII)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            item_for(self).label = args[1].as_ref();
            GaugeState& gs = g_gauges[self];
            gs.interactive = args[2].as_int() != 0;
            gs.max_value = args[3].as_int();
            gs.value = args[4].as_int();
        });
    vm.register_native("javax/microedition/lcdui/Gauge",
        "getValue", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_gauges.find(args[0].as_ref());
            f.push_int(it != g_gauges.end() ? it->second.value : 0);
        });
    vm.register_native("javax/microedition/lcdui/Gauge",
        "setValue", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_gauges[args[0].as_ref()].value = args[1].as_int();
        });
    vm.register_native("javax/microedition/lcdui/Gauge",
        "getMaxValue", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_gauges.find(args[0].as_ref());
            f.push_int(it != g_gauges.end() ? it->second.max_value : 100);
        });
    vm.register_native("javax/microedition/lcdui/Gauge",
        "setMaxValue", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_gauges[args[0].as_ref()].max_value = args[1].as_int();
        });
    vm.register_native("javax/microedition/lcdui/Gauge",
        "isInteractive", "()Z",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_gauges.find(args[0].as_ref());
            f.push_int((it != g_gauges.end() && it->second.interactive) ? 1 : 0);
        });

    // ── DateField ────────────────────────────────────────────────────────────
    // mode: DATE=1, TIME=2, DATE_TIME=3
    struct DateFieldState {
        int mode = 1;
        int64_t millis = 0;            // 0 means "uninitialized" per spec
        bool initialized = false;
    };
    static std::unordered_map<ObjRef, DateFieldState> g_date_fields;
    vm.register_native("javax/microedition/lcdui/DateField",
        "<init>", "(Ljava/lang/String;I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            item_for(self).label = args[1].as_ref();
            g_date_fields[self].mode = args[2].as_int();
        });
    vm.register_native("javax/microedition/lcdui/DateField",
        "<init>", "(Ljava/lang/String;ILjava/util/TimeZone;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            item_for(self).label = args[1].as_ref();
            g_date_fields[self].mode = args[2].as_int();
            // TimeZone arg ignored — we always render in local time
        });
    vm.register_native("javax/microedition/lcdui/DateField",
        "getInputMode", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_date_fields.find(args[0].as_ref());
            f.push_int(it != g_date_fields.end() ? it->second.mode : 1);
        });
    vm.register_native("javax/microedition/lcdui/DateField",
        "setInputMode", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_date_fields[args[0].as_ref()].mode = args[1].as_int();
        });
    vm.register_native("javax/microedition/lcdui/DateField",
        "getDate", "()Ljava/util/Date;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            auto it = g_date_fields.find(args[0].as_ref());
            if (it == g_date_fields.end() || !it->second.initialized) {
                f.push_ref(NULL_REF); return;
            }
            ClassDef* k = v.loader().find_or_stub("java/util/Date");
            f.push_ref(v.heap().alloc_object(k, 0));
            // Date contents not stored here; getTime() returns wall clock per
            // our existing Date.getTime native. Roundtripping would need real
            // Date storage — out of scope for the LCDUI pass.
        });
    vm.register_native("javax/microedition/lcdui/DateField",
        "setDate", "(Ljava/util/Date;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            DateFieldState& ds = g_date_fields[args[0].as_ref()];
            ds.initialized = (args[1].as_ref() != NULL_REF);
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

    // Form/Alert/TextBox/List inherit Displayable.addCommand /
    // setCommandListener at runtime, but the methodref is on the subclass for
    // ~1100 JARs each — duplicate so the scanner and early-bound bytecode see
    // them on the exact class. Use a for-loop pattern (the scanner detects
    // these and expands to one row per class).
    for (const char* klass : {
        "javax/microedition/lcdui/Form",
        "javax/microedition/lcdui/Alert",
        "javax/microedition/lcdui/TextBox",
        "javax/microedition/lcdui/List",
    }) {
        vm.register_native(klass,
            "setCommandListener", "(Ljavax/microedition/lcdui/CommandListener;)V",
            [](VM&, Frame&, std::span<Slot> args) {
                g_command_listeners[args[0].as_ref()] = args[1].as_ref();
            });
        vm.register_noop(klass,
            "addCommand", "(Ljavax/microedition/lcdui/Command;)V",
            "subclass mirror of Displayable.addCommand",
            [](VM&, Frame&, std::span<Slot>) {});
        vm.register_noop(klass,
            "removeCommand", "(Ljavax/microedition/lcdui/Command;)V",
            "subclass mirror of Displayable.removeCommand",
            [](VM&, Frame&, std::span<Slot>) {});
        vm.register_native(klass,
            "setTitle", "(Ljava/lang/String;)V",
            [](VM&, Frame&, std::span<Slot>) {});
        vm.register_native(klass,
            "getTitle", "()Ljava/lang/String;",
            [](VM&, Frame& f, std::span<Slot>) { f.push_ref(NULL_REF); });
    }

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
