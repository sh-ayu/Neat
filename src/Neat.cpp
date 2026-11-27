#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <rapidjson/document.h>
#include <ll/api/event/EventBus.h>
#include <ll/api/event/render/UIRenderEvent.h>
#include <ll/api/event/world/ClientLevelTickEvent.h>
#include <ll/api/memory/Hook.h>
#include <ll/api/mod/RegisterHelper.h>
#include <ll/api/service/TargetedBedrock.h>

#include <mc/client/game/ClientInstance.h>
#include <mc/client/game/IMinecraftGame.h>
#include <mc/client/gui/CaretMeasureData.h>
#include <mc/client/gui/FontHandle.h>
#include <mc/client/gui/TextMeasureData.h>
#include <mc/client/options/IOptionRegistry.h>
#include <mc/client/player/LocalPlayer.h>
#include <mc/client/renderer/BaseActorRenderContext.h>
#include <mc/client/renderer/game/LevelRenderer.h>
#include <mc/client/renderer/game/LevelRendererPlayer.h>
#include <mc/deps/core/math/Color.h>
#include <mc/deps/input/RectangleArea.h>
#include <mc/deps/renderer/Camera.h>
#include <mc/locale/I18n.h>
#include <mc/world/actor/ActorFlags.h>
#include <mc/world/actor/ActorType.h>
#include <mc/world/actor/Mob.h>
#include <mc/world/actor/player/PlayerListEntry.h>
#include <mc/world/level/BlockSource.h>
#include <mc/world/level/Level.h>
#include <mc/world/level/ShapeType.h>
#include <mc/world/phys/HitResult.h>

namespace neat {
namespace {

using ll::event::AfterUIRenderEvent;
using ll::event::ClientLevelTickEvent;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr auto kSnapshotLifetime = 250ms;
constexpr float kMaxVisibilityOriginDrift = 2.0f;
constexpr std::size_t kMaxVisibilityChecks = 64;
constexpr std::size_t kMaxLabels = 24;

struct Settings {
    float maxDistance = 24;
    bool enabled = true;
    bool showPlayers = false;
    bool showArmor = true;
};

struct CameraFrame {
    LevelRendererPlayer const *renderer;
    Clock::time_point captured;
    glm::vec3 right, up, forward;
    float horizontalScale, verticalScale, alpha;
    Vec3 position;
};

struct Label {
    Vec3 previousAnchor, anchor;
    std::string text;
    std::optional<float> healthFraction;
};

struct Collection {
    Level const *level;
    int dimension;
    Clock::time_point captured;
    Vec3 visibilityOrigin;
    std::vector<Label> labels;
};

std::atomic_bool active{false};
std::mutex snapshotMutex;
std::optional<CameraFrame> waitingCamera, renderCamera;
std::shared_ptr<Collection const> collection;

bool finite(Vec3 const &v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

LL_TYPE_INSTANCE_HOOK(CaptureCamera, ll::memory::HookPriority::Highest, LevelRendererPlayer,
                      &LevelRendererPlayer::setupCamera, void, mce::Camera &camera, float alpha) {
    origin(camera, alpha);
    if (!active.load())
        return;
    std::lock_guard lock(snapshotMutex);
    // Auxiliary passes can run between the world camera setup and the entity pass.
    if (camera.viewMatrixStack->stack->empty() || camera.projectionMatrixStack->stack->empty())
        return;

    auto const &view = *camera.viewMatrixStack->top()._m;
    auto const &projection = *camera.projectionMatrixStack->top()._m;
    float const x = projection[0][0], y = projection[1][1];
    if (!std::isfinite(x) || !std::isfinite(y) || x <= 0 || y <= 0 ||
        !(std::abs(projection[2][3] + 1) < 0.001f && std::abs(projection[3][3]) < 0.001f &&
          std::abs(projection[2][0]) < 0.001f && std::abs(projection[2][1]) < 0.001f))
        return;

    // As in PearlTrajectory, the view basis is camera-relative. The entity pass supplies
    // the world origin, including camera offsets in third-person view.
    waitingCamera = CameraFrame{this,
                                Clock::now(),
                                {view[0][0], view[1][0], view[2][0]},
                                {view[0][1], view[1][1], view[2][1]},
                                {-view[0][2], -view[1][2], -view[2][2]},
                                x,
                                y,
                                std::isfinite(alpha) ? std::clamp(alpha, 0.0f, 1.0f) : 1.0f,
                                {}};
}

LL_TYPE_INSTANCE_HOOK(CapturePosition, ll::memory::HookPriority::Normal, LevelRendererPlayer,
                      &LevelRendererPlayer::$renderEntityEffects, void, BaseActorRenderContext &context) {
    origin(context);
    if (!active.load())
        return;
    std::lock_guard lock(snapshotMutex);
    if (!waitingCamera || waitingCamera->renderer != this)
        return;
    if (!context.mImpl || Clock::now() - waitingCamera->captured > kSnapshotLifetime)
        return;
    auto const &position = context.mImpl->mCameraPosition.get();
    if (!finite(position))
        return;
    waitingCamera->position = position;
    renderCamera = std::exchange(waitingCamera, std::nullopt);
}

bool visibleHud(ClientInstance &client) {
    return client.getMinecraftGame().isInGame() && !client.getOptions().getHideHud() && !client.isShowingMenu() &&
           client.getTopScreenName() == "hud_screen";
}

LevelRendererPlayer const *worldRenderer(ClientInstance &client) {
    auto *renderer = client.getLevelRenderer();
    return renderer ? renderer->mLevelRendererPlayer.get() : nullptr;
}

bool isMobForLabel(Actor &actor, ActorType type) {
    // Bedrock also sets the Mob category bit on display and utility actors.
    switch (type) {
    case ActorType::Npc:
    case ActorType::ArmorStand:
    case ActorType::Agent:
    case ActorType::TripodCamera:
    case ActorType::ElderGuardianGhost:
    case ActorType::Player:
        return false;
    default:
        break;
    }
    if (!actor.hasType(ActorType::Mob) || actor.mInvulnerable)
        return false;
    // Servers also reuse living actor types for holograms and lobby buttons.
    if (actor.getNameTag().empty())
        return true;
    return !actor.getStatusFlag(ActorFlags::Noai) &&
           !(actor.getStatusFlag(ActorFlags::AlwaysShowName) &&
             (!actor.getStatusFlag(ActorFlags::HasGravity) || !actor.getStatusFlag(ActorFlags::HasCollision)));
}

std::string mobDisplayName(Actor &actor) {
    if (auto const &custom = actor.getNameTag(); !custom.empty())
        return custom;
    auto const &type = actor.getTypeName();
    auto &language = getI18n();
    auto const locale = language.getCurrentLanguage().get();
    auto const key = "entity." + type + ".name";
    auto translated = language.get(key, locale);
    if (!translated.empty() && translated != key)
        return translated;
    auto const name = type.starts_with("minecraft:") ? type.substr(10) : type;
    if (name != type) {
        auto const legacyKey = "entity." + name + ".name";
        translated = language.get(legacyKey, locale);
        if (!translated.empty() && translated != legacyKey)
            return translated;
    }
    return name;
}

void collect(ClientLevelTickEvent &event, Settings const &settings) {
    auto client = ll::service::getClientInstance();
    auto *player = client ? client->getLocalPlayer() : nullptr;
    if (!player || !visibleHud(*client) || &player->getLevel() != &event.level()) {
        std::lock_guard lock(snapshotMutex);
        collection.reset();
        return;
    }

    std::optional<CameraFrame> camera;
    {
        std::lock_guard lock(snapshotMutex);
        camera = renderCamera;
    }
    if (!camera || camera->renderer != worldRenderer(*client) || Clock::now() - camera->captured > kSnapshotLifetime)
        return;

    struct Candidate {
        Actor *actor;
        Vec3 anchor;
        float distanceSquared;
        int health, maxHealth;
        std::string const *playerName;
    };

    int const dimension = static_cast<int>(player->getDimensionId());
    float const limitSquared = settings.maxDistance * settings.maxDistance;
    std::vector<Candidate> nearby;
    auto const addCandidate = [&](Actor *actor, std::string const *playerName) {
        if (!actor || actor == player || actor->mRemoved || actor->isInvisible() ||
            static_cast<int>(actor->getDimensionId()) != dimension)
            return;

        auto const type = actor->getEntityTypeId();
        if (playerName) {
            if (type != ActorType::Player || actor->isDead() || actor->isSneaking())
                return;
        } else if (actor->isPlayer() || !isMobForLabel(*actor, type) || !actor->isAlive()) {
            return;
        }

        auto const &bounds = actor->getAABB();
        if (!(bounds.max.y - bounds.min.y > 0.01f && bounds.max.x - bounds.min.x > 0.01f &&
              bounds.max.z - bounds.min.z > 0.01f))
            return;
        Vec3 const anchor{(bounds.min.x + bounds.max.x) * 0.5f, bounds.max.y + 0.14f,
                          (bounds.min.z + bounds.max.z) * 0.5f};
        auto const delta = anchor - camera->position;
        float const distanceSquared = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
        if (!std::isfinite(distanceSquared) || distanceSquared > limitSquared)
            return;

        int health = actor->getHealth(), maxHealth = actor->getMaxHealth();
        if (health <= 0 || maxHealth <= 0 || maxHealth == std::numeric_limits<int>::max()) {
            if (!playerName)
                return;
            health = maxHealth = 0;
        }
        // Small slimes and magma cubes are genuine one-HP mobs.
        if (!playerName && maxHealth == 1 && type != ActorType::Slime && type != ActorType::LavaSlime)
            return;
        nearby.push_back({actor, anchor, distanceSquared, health, maxHealth, playerName});
    };
    for (auto const &entity : event.level().getEntities()) {
        if (auto actor = entity.tryUnwrap())
            addCandidate(&*actor, nullptr);
    }
    if (settings.showPlayers) {
        for (auto const &[uuid, entry] : event.level().getPlayerList()) {
            if (!entry.mName.get().empty())
                addCandidate(event.level().fetchEntity(entry.mId.get(), false), &entry.mName.get());
        }
    }

    auto const rayCount = std::min(nearby.size(), kMaxVisibilityChecks);
    std::partial_sort(nearby.begin(), nearby.begin() + rayCount, nearby.end(),
                      [](auto const &a, auto const &b) { return a.distanceSquared < b.distanceSquared; });

    auto snapshot =
        std::make_shared<Collection>(Collection{&event.level(), dimension, Clock::now(), camera->position, {}});
    snapshot->labels.reserve(rayCount);
    auto const &blocks = player->getDimensionBlockSourceConst();
    for (std::size_t i = 0; i < rayCount; ++i) {
        auto const &candidate = nearby[i];
        auto &actor = *candidate.actor;
        auto const &bounds = actor.getAABB();
        int const raySteps = 2 * static_cast<int>(std::ceil(settings.maxDistance)) + 16;
        auto const blockedAt = [&](float height) {
            Vec3 const point{candidate.anchor.x, std::lerp(bounds.min.y, bounds.max.y, height), candidate.anchor.z};
            return !finite(point) || blocks.clip(
                camera->position, point, false, ShapeType::Collision, raySteps, false, false, player,
                [](BlockSource const &, Block const &, bool) { return true; }, false
            ).mType == HitResultType::Tile;
        };
        // Sample the mob itself so a ceiling above a visible mob does not hide its bar.
        if (blockedAt(0.85f) && blockedAt(0.50f))
            continue;

        int const health = std::min(candidate.health, candidate.maxHealth);
        auto name = candidate.playerName ? *candidate.playerName : mobDisplayName(actor);
        // Custom names can contain line breaks. Keep the text and bar on one row.
        std::replace(name.begin(), name.end(), '\n', ' ');
        std::replace(name.begin(), name.end(), '\r', ' ');
        std::optional<float> fraction;
        if (candidate.maxHealth > 0)
            fraction = static_cast<float>(health) / candidate.maxHealth;
        auto text = name + (fraction ? "  " + std::to_string(health) + "/" + std::to_string(candidate.maxHealth)
                                     : "  HP ?");
        if (settings.showArmor && (!candidate.playerName || actor.isPlayer())) {
            int const armor = static_cast<Mob &>(actor).getArmorValue();
            if (armor > 0)
                text += "  [" + std::to_string(armor) + "]";
        }

        Vec3 const previous = candidate.anchor + (actor.getInterpolatedPosition(0) - actor.getPosition());
        snapshot->labels.push_back({finite(previous) ? previous : candidate.anchor, candidate.anchor, std::move(text),
                                    fraction});
    }

    std::lock_guard lock(snapshotMutex);
    if (active.load())
        collection = std::move(snapshot);
}

RectangleArea rectangle(float left, float top, float right, float bottom) {
    RectangleArea area;
    area._x0 = left;
    area._y0 = top;
    area._x1 = right;
    area._y1 = bottom;
    return area;
}

mce::Color healthColor(float fraction) {
    float const red = std::min(1.0f, 2 * (1 - fraction));
    float const green = std::min(1.0f, 2 * fraction);
    return {0.30f + 0.65f * red, 0.24f + 0.63f * green, 0.22f};
}

void draw(AfterUIRenderEvent &event, Settings const &settings) {
    if (event.screenView().getScreenName() != "hud_screen")
        return;
    auto client = ll::service::getClientInstance();
    auto *player = client ? client->getLocalPlayer() : nullptr;
    if (!player || !visibleHud(*client))
        return;

    std::optional<CameraFrame> camera;
    std::shared_ptr<Collection const> snapshot;
    {
        std::lock_guard lock(snapshotMutex);
        camera = renderCamera;
        snapshot = collection;
    }
    auto const now = Clock::now();
    if (!camera || !snapshot || snapshot->labels.empty() || camera->renderer != worldRenderer(*client) ||
        snapshot->level != &player->getLevel() || snapshot->dimension != static_cast<int>(player->getDimensionId()) ||
        now - camera->captured > kSnapshotLifetime || now - snapshot->captured > kSnapshotLifetime)
        return;

    auto const shift = camera->position - snapshot->visibilityOrigin;
    // A normal frame may advance the camera beyond the last tick's occlusion origin.
    if (shift.x * shift.x + shift.y * shift.y + shift.z * shift.z >
        kMaxVisibilityOriginDrift * kMaxVisibilityOriginDrift)
        return;

    auto &ctx = event.uiRenderContext();
    auto const clip = ctx.getFullClippingRectangle();
    float const left = clip._x0, top = clip._y0, right = clip._x1, bottom = clip._y1;
    float const width = right - left, height = bottom - top;
    if (!(width > 0 && height > 0))
        return;

    struct ProjectedLabel {
        Label const *label;
        float x, y, depth;
    };
    std::vector<ProjectedLabel> projected;
    projected.reserve(snapshot->labels.size());
    for (auto const &label : snapshot->labels) {
        auto const relative =
            label.previousAnchor + (label.anchor - label.previousAnchor) * camera->alpha - camera->position;
        glm::vec3 const offset{relative.x, relative.y, relative.z};
        float const depth = glm::dot(offset, camera->forward);
        if (!(depth > 0.10f) || glm::dot(offset, offset) > settings.maxDistance * settings.maxDistance)
            continue;
        float const x = glm::dot(offset, camera->right) * camera->horizontalScale / depth;
        float const y = glm::dot(offset, camera->up) * camera->verticalScale / depth;
        if (!(std::abs(x) < 1 && std::abs(y) < 1))
            continue;
        projected.push_back({&label, left + (1 + x) * width * 0.5f, top + (1 - y) * height * 0.5f, depth});
    }
    if (projected.empty())
        return;

    std::sort(projected.begin(), projected.end(), [](auto const &a, auto const &b) { return a.depth < b.depth; });
    auto fontHandle = client->getFontHandle();
    auto const &font = fontHandle.mDefaultFont.get();
    bool drew = false;
    auto const count = std::min(projected.size(), kMaxLabels);
    for (std::size_t i = count; i > 0; --i) {
        auto const &item = projected[i - 1];
        auto const &label = *item.label;
        float const scale = std::clamp(height * camera->verticalScale * 0.021f / item.depth, 0.62f, 0.94f);
        float const textWidth = font ? static_cast<float>(ctx.getLineLength(*font, label.text, scale, false))
                                     : static_cast<float>(label.text.size()) * 6 * scale;
        float const halfWidth = std::max(25 * scale, textWidth * 0.5f + 3);
        float const x0 = item.x - halfWidth, x1 = item.x + halfWidth;
        float const y0 = item.y - 15 * scale, y1 = item.y;
        if (x0 < left || x1 > right || y0 < top || y1 > bottom)
            continue;

        ctx.fillRectangle(rectangle(x0, y0, x1, y1), mce::Color(12, 15, 18), 0.85f);
        float const trackLeft = x0 + 2, trackRight = x1 - 2;
        float const trackTop = y1 - 4 * scale, trackBottom = y1 - scale;
        ctx.fillRectangle(rectangle(trackLeft, trackTop, trackRight, trackBottom), mce::Color(59, 62, 66), 1);
        if (label.healthFraction) {
            auto const fraction = *label.healthFraction;
            ctx.fillRectangle(rectangle(trackLeft, trackTop, std::lerp(trackLeft, trackRight, fraction), trackBottom),
                              healthColor(fraction), 1);
        }

        auto const textRect = rectangle(x0, y0 + scale, x1, trackTop);
        TextMeasureData const textData{scale, 0, true, false, false, ui::TextAlignment::Center};
        CaretMeasureData const caretData{0, false};
        mce::Color const white(255, 255, 255);
        if (font) {
            ctx.drawText(*font, textRect, std::string_view(label.text), white, 1, ui::TextAlignment::Center, textData,
                         caretData);
        } else {
            ctx.drawDebugText(textRect, std::string(label.text), white, 1, ui::TextAlignment::Center, textData,
                              caretData);
        }
        drew = true;
    }
    if (drew)
        ctx.flushText(0.016f, std::nullopt);
}

} // namespace

class Mod {
    std::weak_ptr<ll::mod::NativeMod> self;
    ll::event::ListenerPtr tickListener, uiListener;
    bool cameraHook = false, positionHook = false;

  public:
    bool load() {
        self = ll::mod::NativeMod::current();
        return !self.expired();
    }

    bool enable() {
        auto mod = self.lock();
        if (!mod)
            return false;
        Settings settings;
        auto const path = mod->getConfigDir() / "config.json";
        try {
            std::filesystem::create_directories(path.parent_path());
            if (!std::filesystem::exists(path)) {
                std::ofstream output(path);
                output << "{\n  \"enabled\": true,\n  \"maxDistance\": 24,\n"
                          "  \"showPlayers\": false,\n  \"showArmor\": true\n}\n";
                if (!output)
                    mod->getLogger().warn("Could not write {}", path.string());
            }
            std::ifstream input(path);
            std::string const json{std::istreambuf_iterator<char>{input}, {}};
            rapidjson::Document config;
            config.Parse(json.c_str());
            if (config.IsObject()) {
                auto const readBool = [&](char const *key, bool &value) {
                    auto item = config.FindMember(key);
                    if (item != config.MemberEnd() && item->value.IsBool())
                        value = item->value.GetBool();
                };
                readBool("enabled", settings.enabled);
                readBool("showPlayers", settings.showPlayers);
                readBool("showArmor", settings.showArmor);
                auto distance = config.FindMember("maxDistance");
                if (distance != config.MemberEnd() && distance->value.IsNumber()) {
                    settings.maxDistance = static_cast<float>(std::clamp(distance->value.GetDouble(), 4.0, 48.0));
                }
            } else {
                mod->getLogger().warn("Could not read {}; using defaults", path.string());
            }
        } catch (std::exception const &error) {
            mod->getLogger().warn("Could not load settings: {}", error.what());
        }
        if (!settings.enabled)
            return true;

        try {
            cameraHook = CaptureCamera::hook() == 0;
            positionHook = cameraHook && CapturePosition::hook() == 0;
            if (positionHook) {
                auto &bus = ll::event::EventBus::getInstance();
                auto const config = settings;
                tickListener = bus.emplaceListener<ClientLevelTickEvent>(
                    [config](ClientLevelTickEvent &event) {
                        if (!active.load())
                            return;
                        try {
                            collect(event, config);
                        } catch (...) {
                            std::lock_guard lock(snapshotMutex);
                            collection.reset();
                        }
                    },
                    ll::event::EventPriority::Normal, self);
                uiListener = bus.emplaceListener<AfterUIRenderEvent>(
                    [config](AfterUIRenderEvent &event) {
                        if (!active.load())
                            return;
                        try {
                            draw(event, config);
                        } catch (...) {
                            // Leave the vanilla render pass running if the overlay fails.
                        }
                    },
                    ll::event::EventPriority::Normal, self);
                if (tickListener && uiListener) {
                    active.store(true);
                    mod->getLogger().info("Neat overlay enabled");
                    return true;
                }
            }
        } catch (std::exception const &error) {
            mod->getLogger().error("Neat initialization failed: {}", error.what());
        }
        mod->getLogger().error("Could not install Neat's camera hooks or event listeners");
        disable();
        return false;
    }

    bool disable() {
        active.store(false);
        auto &bus = ll::event::EventBus::getInstance();
        if (tickListener)
            bus.removeListener<ClientLevelTickEvent>(std::exchange(tickListener, {}));
        if (uiListener)
            bus.removeListener<AfterUIRenderEvent>(std::exchange(uiListener, {}));
        if (positionHook)
            CapturePosition::unhook();
        if (cameraHook)
            CaptureCamera::unhook();
        positionHook = cameraHook = false;
        std::lock_guard lock(snapshotMutex);
        waitingCamera.reset();
        renderCamera.reset();
        collection.reset();
        return true;
    }
};

Mod entry;
} // namespace neat

LL_REGISTER_MOD(neat::Mod, neat::entry)
