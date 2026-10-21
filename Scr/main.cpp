#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/utils/web.hpp>
#include <set>

using namespace geode::prelude;

// Objetos permitidos (IDs de GD 2.2). Edita esta lista para ampliar.
static const std::set<int> ALLOWED = {
    1, 8, 39,                          // bloque, pincho, pincho pequeno
    35, 140, 67, 1332,                 // pads: amarillo, rosa, azul, rojo
    36, 141, 84, 1022, 1330, 1333,     // orbs: amarillo, rosa, azul, verde, negro, rojo
    10, 11, 12, 13, 47, 111, 660, 745, 1331, // portales gravedad/modos
    99, 101,                           // mini / normal
    200, 201, 202, 203, 1334           // portales de velocidad
};

static const char* SYSTEM_PROMPT = R"(You are Layout Agent, a Geometry Dash level-design engine inside the editor.
Output ONLY a JSON object, no prose, no markdown: {"objects":[[id,x,y,rot],...]}
Coordinates are in BLOCK UNITS (1 block = 1 unit), x grows to the right, y grows up.
A block centered on the ground has y=0.5. Center of the first column is x=0.5.
Allowed IDs: 1 block, 8 spike, 39 small spike, 35 yellow pad, 140 pink pad, 67 blue pad, 1332 red pad,
36 yellow orb, 141 pink orb, 84 blue orb, 1022 green orb, 1330 black orb, 1333 red orb,
10 gravity down portal, 11 gravity up portal, 12 cube, 13 ship, 47 ball, 111 ufo, 660 wave, 745 robot, 1331 spider,
99 mini portal, 101 normal-size portal, 200 0.5x speed, 201 1x, 202 2x, 203 3x, 1334 4x speed.
rot is degrees (0, 90, 180, 270). Spikes point up at rot 0 and must sit on a block or the floor.
Rules: every jump must be physically beatable at the given speed; leave a floor/ceiling for the active gamemode;
use rhythm, variety, ramps made of stacked blocks, orb chains and drops when asked for a hard or modern layout.
MODE layout: create a layout from scratch starting at x=0 using the request.
MODE decorate: you receive SELECTED objects as [id,x,y,rot] in block units. Add decoration blocks around/over them
(shading, trims, details) matching the request; do NOT repeat the selected objects and do NOT block the player path.
Never exceed the object limit given.)";

static std::vector<Ref<GameObject>> s_lastBatch;

class AgentPopup : public Popup<EditorUI*> {
protected:
    EditorUI* m_ui = nullptr;
    TextInput* m_input = nullptr;
    CCLabelBMFont* m_status = nullptr;
    ButtonSprite* m_modeSpr = nullptr;
    int m_mode = 0; // 0 layout, 1 decorar seleccion
    EventListener<web::WebTask> m_listener;

    bool setup(EditorUI* ui) override {
        m_ui = ui;
        this->setTitle("Layout Agent");

        m_input = TextInput::create(330.f, "Describe lo que quieres (cualquier idioma)", "chatFont.fnt");
        m_input->setMaxCharCount(400);
        m_mainLayer->addChildAtPosition(m_input, Anchor::Center, {0.f, 40.f});

        m_status = CCLabelBMFont::create("Listo", "bigFont.fnt");
        m_status->setScale(0.4f);
        m_mainLayer->addChildAtPosition(m_status, Anchor::Center, {0.f, -5.f});

        auto menu = CCMenu::create();
        menu->setContentSize({340.f, 40.f});
        menu->setLayout(RowLayout::create()->setGap(8.f));

        m_modeSpr = ButtonSprite::create("Layout", 80, true, "bigFont.fnt", "GJ_button_04.png", 30.f, 0.6f);
        menu->addChild(CCMenuItemExt::createSpriteExtra(m_modeSpr, [this](auto) {
            m_mode = (m_mode + 1) % 2;
            m_modeSpr->setString(m_mode == 0 ? "Layout" : "Decorar");
        }));
        menu->addChild(CCMenuItemExt::createSpriteExtra(
            ButtonSprite::create("Generar", 80, true, "bigFont.fnt", "GJ_button_01.png", 30.f, 0.6f),
            [this](auto) { this->onGenerate(); }));
        menu->addChild(CCMenuItemExt::createSpriteExtra(
            ButtonSprite::create("Deshacer", 80, true, "bigFont.fnt", "GJ_button_06.png", 30.f, 0.6f),
            [this](auto) { this->onUndo(); }));
        menu->updateLayout();
        m_mainLayer->addChildAtPosition(menu, Anchor::Bottom, {0.f, 35.f});

        m_listener.bind([this](web::WebTask::Event* e) {
            if (auto* res = e->getValue()) this->onResponse(res);
        });
        return true;
    }

    void setStatus(std::string const& s) { m_status->setString(s.c_str()); }

    void onUndo() {
        auto layer = m_ui->m_editorLayer;
        for (auto& o : s_lastBatch) if (o) layer->removeObject(o, false);
        setStatus(fmt::format("Quitados {} objetos", s_lastBatch.size()));
        s_lastBatch.clear();
    }

    void onGenerate() {
        auto key = Mod::get()->getSettingValue<std::string>("api-key");
        if (key.empty()) return setStatus("Falta la API key (ajustes del mod)");
        auto model = Mod::get()->getSettingValue<std::string>("model");
        auto limit = Mod::get()->getSettingValue<int64_t>("max-objects");

        std::string user = fmt::format("MODE {}\nObject limit: {}\nRequest: {}\n",
            m_mode == 0 ? "layout" : "decorate", limit, m_input->getString());

        if (m_mode == 1) {
            auto sel = m_ui->getSelectedObjects();
            if (!sel || sel->count() == 0) return setStatus("Selecciona objetos primero");
            std::string list = "SELECTED: [";
            int n = 0;
            for (auto* o : CCArrayExt<GameObject*>(sel)) {
                if (n++ >= 400) break;
                list += fmt::format("[{},{:.1f},{:.1f},{}],", o->m_objectID,
                    o->getPositionX() / 30.f, o->getPositionY() / 30.f, (int)o->getRotation());
            }
            list += "]\n";
            user += list;
        }

        matjson::Value msg;
        msg["role"] = "user";
        msg["content"] = user;
        matjson::Value msgs = std::vector<matjson::Value>{msg};
        matjson::Value body;
        body["model"] = model;
        body["max_tokens"] = 16000;
        body["system"] = std::string(SYSTEM_PROMPT);
        body["messages"] = msgs;

        web::WebRequest req;
        req.header("x-api-key", key);
        req.header("anthropic-version", "2023-06-01");
        req.timeout(std::chrono::seconds(180));
        req.bodyJSON(body);
        m_listener.setFilter(req.post("https://api.anthropic.com/v1/messages"));
        setStatus("Pensando... (no cierres esta ventana)");
    }

    void onResponse(web::WebResponse* res) {
        if (!res->ok()) return setStatus(fmt::format("Error HTTP {}", res->code()));
        auto j = res->json();
        if (!j) return setStatus("Respuesta invalida");
        auto root = j.unwrap();
        std::string text = root["content"][0]["text"].asString().unwrapOr("");
        auto a = text.find('{');
        auto b = text.rfind('}');
        if (a == std::string::npos || b == std::string::npos) return setStatus("La IA no devolvio JSON");
        auto parsed = matjson::parse(text.substr(a, b - a + 1));
        if (!parsed) return setStatus("JSON roto, intenta de nuevo");
        auto arr = parsed.unwrap()["objects"].asArray();
        if (!arr) return setStatus("Sin objetos");

        auto layer = m_ui->m_editorLayer;
        auto ol = layer->m_objectLayer;
        float ox = 0.f;
        if (m_mode == 0) {
            float s = ol->getScale();
            ox = std::round((-ol->getPositionX() / s) / 30.f) * 30.f;
        }
        auto limit = Mod::get()->getSettingValue<int64_t>("max-objects");
        s_lastBatch.clear();
        for (auto& o : arr.unwrap()) {
            if ((int64_t)s_lastBatch.size() >= limit) break;
            if (!o.isArray()) continue;
            auto const& v = o.asArray().unwrap();
            if (v.size() < 3) continue;
            int id = (int)v[0].asInt().unwrapOr(0);
            if (!ALLOWED.count(id)) continue;
            float gx = (float)v[1].asDouble().unwrapOr(0);
            float gy = (float)v[2].asDouble().unwrapOr(0);
            float rot = v.size() > 3 ? (float)v[3].asDouble().unwrapOr(0) : 0.f;
            auto obj = layer->createObject(id, CCPoint{ox + gx * 30.f, gy * 30.f}, false);
            if (!obj) continue;
            obj->setRotation(rot);
            s_lastBatch.push_back(obj);
        }
        setStatus(fmt::format("Listo: {} objetos", s_lastBatch.size()));
    }

public:
    static AgentPopup* create(EditorUI* ui) {
        auto ret = new AgentPopup();
        if (ret->initAnchored(380.f, 220.f, ui)) { ret->autorelease(); return ret; }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
};

class $modify(AgentEditorUI, EditorUI) {
    bool init(LevelEditorLayer* layer) {
        if (!EditorUI::init(layer)) return false;
        auto spr = ButtonSprite::create("IA", 40, true, "bigFont.fnt", "GJ_button_02.png", 30.f, 0.6f);
        auto btn = CCMenuItemExt::createSpriteExtra(spr, [this](auto) { AgentPopup::create(this)->show(); });
        if (auto menu = this->getChildByID("undo-menu")) {
            menu->addChild(btn);
            menu->updateLayout();
        }
        return true;
    }
};
