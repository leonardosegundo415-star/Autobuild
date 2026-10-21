#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <cctype>
#include <cmath>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace geode::prelude;

// Objetos permitidos (IDs de GD 2.2). Edita esta lista para ampliar.
static const std::set<int> ALLOWED = {
    1, 8, 39,                          // bloque, pincho, pincho pequeno
    35, 140, 67, 1332,                 // pads: amarillo, rosa, azul, rojo
    36, 141, 84, 1022, 1330, 1333,     // orbs: amarillo, rosa, azul, verde, negro, rojo
    10, 11, 12, 13, 47, 111, 660, 745, 1331, // portales gravedad/modos
    99, 101,                           // normal / mini
    200, 201, 202, 203, 1334           // portales de velocidad
};

static const char* SYSTEM_PROMPT = R"(You are Layout Agent, a Geometry Dash level-design engine inside the editor.
Output ONLY a JSON object, no prose, no markdown: {"objects":[[id,x,y,rot],...]}
Coordinates are in BLOCK UNITS (1 block = 1 unit), x grows to the right, y grows up.
A block centered on the ground has y=0.5. Center of the first column is x=0.5.
Allowed IDs: 1 block, 8 spike, 39 small spike, 35 yellow pad, 140 pink pad, 67 blue pad, 1332 red pad,
36 yellow orb, 141 pink orb, 84 blue orb, 1022 green orb, 1330 black orb, 1333 red orb,
10 gravity down portal, 11 gravity up portal, 12 cube, 13 ship, 47 ball, 111 ufo, 660 wave, 745 robot, 1331 spider,
99 normal-size portal, 101 mini portal, 200 0.5x speed, 201 1x, 202 2x, 203 3x, 1334 4x speed.
rot is degrees (0, 90, 180, 270). Spikes point up at rot 0 and must sit on a block or the floor.
Rules: every jump must be physically beatable at the given speed; leave a floor/ceiling for the active gamemode;
use rhythm, variety, ramps made of stacked blocks, orb chains and drops when asked for a hard or modern layout.
MODE layout: create a layout from scratch starting at x=0 using the request.
MODE decorate: you receive SELECTED objects as [id,x,y,rot] in block units. Add decoration blocks around/over them
(shading, trims, details) matching the request; do NOT repeat the selected objects and do NOT block the player path.
Never exceed the object limit given.)";

static std::vector<Ref<GameObject>> s_lastBatch;
static LevelEditorLayer* s_batchLayer = nullptr; // editor al que pertenece s_lastBatch

class AgentPopup : public Popup {
protected:
    EditorUI* m_ui = nullptr;
    TextInput* m_input = nullptr;
    CCLabelBMFont* m_status = nullptr;
    ButtonSprite* m_modeSpr = nullptr;
    int m_mode = 0; // 0 layout, 1 decorar seleccion
    async::TaskHolder<web::WebResponse> m_task;

    bool init(EditorUI* ui) {
        if (!Popup::init(380.f, 220.f)) return false;
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

        return true;
    }

    void setStatus(std::string const& s) { m_status->setString(s.c_str()); }

    void onUndo() {
        auto layer = m_ui->m_editorLayer;
        if (layer != s_batchLayer || s_lastBatch.empty()) {
            s_lastBatch.clear();
            s_batchLayer = nullptr;
            return setStatus("Nada que deshacer");
        }
        auto count = s_lastBatch.size();
        for (auto& o : s_lastBatch) if (o) layer->removeObject(o, false);
        s_lastBatch.clear();
        s_batchLayer = nullptr;
        setStatus(fmt::format("Quitados {} objetos", count));
    }

    void onGenerate() {
        auto key = Mod::get()->getSettingValue<std::string>("api-key");
        while (!key.empty() && std::isspace((unsigned char)key.back())) key.pop_back();
        size_t lead = 0;
        while (lead < key.size() && std::isspace((unsigned char)key[lead])) ++lead;
        key = key.substr(lead);
        if (key.empty()) return setStatus("Falta la API key (ajustes del mod)");
        auto model = Mod::get()->getSettingValue<std::string>("model");
        auto limit = Mod::get()->getSettingValue<int64_t>("max-objects");

        std::string request(m_input->getString());
        std::string user = fmt::format("MODE {}\nObject limit: {}\nRequest: {}\n",
            m_mode == 0 ? "layout" : "decorate", limit, request);

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
            if (!list.empty() && list.back() == ',') list.pop_back();
            list += "]\n";
            user += list;
        }

        // Clave sk-ant-... = Claude (de pago). Cualquier otra (AIza...) = Gemini (plan gratis).
        bool useClaude = key.rfind("sk-ant-", 0) == 0;

        web::WebRequest req;
        req.timeout(std::chrono::seconds(180));
        std::string url;

        if (useClaude) {
            matjson::Value msg = matjson::makeObject({
                {"role", "user"},
                {"content", user}
            });
            matjson::Value msgs = std::vector<matjson::Value>{msg};
            matjson::Value body = matjson::makeObject({
                {"model", model},
                {"max_tokens", 16000},
                {"system", std::string(SYSTEM_PROMPT)},
                {"messages", msgs}
            });
            req.header("x-api-key", key);
            req.header("anthropic-version", "2023-06-01");
            req.bodyJSON(body);
            url = "https://api.anthropic.com/v1/messages";
        } else {
            if (model.rfind("gemini", 0) != 0) model = "gemini-2.5-flash";
            matjson::Value sysPart = matjson::makeObject({{"text", std::string(SYSTEM_PROMPT)}});
            matjson::Value sysParts = std::vector<matjson::Value>{sysPart};
            matjson::Value sysInstr = matjson::makeObject({{"parts", sysParts}});
            matjson::Value userPart = matjson::makeObject({{"text", user}});
            matjson::Value userParts = std::vector<matjson::Value>{userPart};
            matjson::Value userMsg = matjson::makeObject({{"role", "user"}, {"parts", userParts}});
            matjson::Value contents = std::vector<matjson::Value>{userMsg};
            matjson::Value genCfg = matjson::makeObject({
                {"maxOutputTokens", 16000},
                {"responseMimeType", "application/json"}
            });
            matjson::Value body = matjson::makeObject({
                {"system_instruction", sysInstr},
                {"contents", contents},
                {"generationConfig", genCfg}
            });
            req.header("x-goog-api-key", key);
            req.bodyJSON(body);
            url = fmt::format("https://generativelanguage.googleapis.com/v1beta/models/{}:generateContent", model);
        }

        // Cerrar el popup cancela la peticion (TaskHolder se destruye con el popup).
        m_task.spawn(
            req.post(url.c_str()),
            [this](web::WebResponse res) { this->onResponse(std::move(res)); }
        );
        setStatus("Pensando... (no cierres esta ventana)");
    }

    void onResponse(web::WebResponse res) {
        if (!res.ok()) {
            int code = res.code();
            log::error("API error {}: {}", code, res.string().unwrapOr(""));
            if (code == 401 || code == 403) return setStatus("API key invalida");
            if (code == 404) return setStatus("Modelo no encontrado (revisa ajustes)");
            if (code == 429) return setStatus("Limite alcanzado, espera 1 minuto");
            return setStatus(fmt::format("Error HTTP {}", code));
        }
        auto json = res.json();
        if (json.isErr()) return setStatus("Respuesta invalida");
        auto root = json.unwrap();

        std::string text;
        auto cands = root["candidates"]; // formato Gemini
        if (cands.isArray() && cands.size() > 0) {
            auto parts = cands[(size_t)0]["content"]["parts"];
            if (parts.isArray()) {
                for (size_t i = 0; i < parts.size(); ++i) {
                    text += parts[i]["text"].asString().unwrapOr("");
                }
            }
        } else {
            auto content = root["content"]; // formato Claude
            if (content.isArray()) {
                for (size_t i = 0; i < content.size(); ++i) {
                    auto& block = content[i];
                    if (block["type"].asString().unwrapOr("") == "text") {
                        text += block["text"].asString().unwrapOr("");
                    }
                }
            }
        }

        auto a = text.find('{');
        auto b = text.rfind('}');
        if (a == std::string::npos || b == std::string::npos || b < a) {
            return setStatus("La IA no devolvio JSON");
        }
        auto parsed = matjson::parse(text.substr(a, b - a + 1));
        if (parsed.isErr()) return setStatus("JSON roto, intenta de nuevo");
        auto objects = parsed.unwrap()["objects"];
        if (!objects.isArray()) return setStatus("Sin objetos");

        auto layer = m_ui->m_editorLayer;
        auto ol = layer->m_objectLayer;
        float ox = 0.f;
        if (m_mode == 0) {
            float s = ol->getScale();
            ox = std::round((-ol->getPositionX() / s) / 30.f) * 30.f;
        }
        auto limit = Mod::get()->getSettingValue<int64_t>("max-objects");
        s_lastBatch.clear();
        s_batchLayer = layer;
        for (size_t i = 0; i < objects.size(); ++i) {
            if ((int64_t)s_lastBatch.size() >= limit) break;
            auto& o = objects[i];
            if (!o.isArray() || o.size() < 3) continue;
            int id = (int)o[0].asInt().unwrapOr(0);
            if (!ALLOWED.count(id)) continue;
            float gx = (float)o[1].asDouble().unwrapOr(0);
            float gy = (float)o[2].asDouble().unwrapOr(0);
            float rot = o.size() > 3 ? (float)o[3].asDouble().unwrapOr(0) : 0.f;
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
        if (ret->init(ui)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

class $modify(AgentEditorUI, EditorUI) {
    bool init(LevelEditorLayer* layer) {
        if (!EditorUI::init(layer)) return false;
        auto spr = ButtonSprite::create("IA", 40, true, "bigFont.fnt", "GJ_button_02.png", 30.f, 0.6f);
        auto btn = CCMenuItemExt::createSpriteExtra(spr, [this](auto) {
            if (auto popup = AgentPopup::create(this)) popup->show();
        });
        if (auto menu = this->getChildByID("undo-menu")) {
            menu->addChild(btn);
            menu->updateLayout();
        } else {
            // Sin Node IDs instalado el menu no existe: usamos uno propio para que el boton se vea.
            auto win = CCDirector::get()->getWinSize();
            auto fallback = CCMenu::create();
            fallback->setPosition({win.width - 30.f, win.height - 90.f});
            fallback->addChild(btn);
            this->addChild(fallback, 100);
        }
        return true;
    }
};
