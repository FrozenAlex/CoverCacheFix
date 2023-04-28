#include "main.hpp"
#include "GlobalNamespace/PlayerData.hpp"
#include "questui/shared/CustomTypes/Components/MainThreadScheduler.hpp"
#include "questui/shared/BeatSaberUI.hpp"
#include "System/Threading/Tasks/TaskCanceledException.hpp"
#include "GlobalNamespace/IBeatmapLevelPack.hpp"
#include "GlobalNamespace/IBeatmapLevel.hpp"
#include "GlobalNamespace/IBeatmapLevelData.hpp"
#include "System/Action.hpp"
#include "System/Func_1.hpp"
#include "System/Func_2.hpp"
#include "System/Action_1.hpp"
#include "System/Threading/Tasks/Task_1.hpp"
#include "System/IO/Path.hpp"
#include "System/IO/File.hpp"
#include "UnityEngine/Object.hpp"
#include "custom-types/shared/delegate.hpp"
#include "UnityEngine/Texture2D.hpp"
#include "GlobalNamespace/IDifficultyBeatmap.hpp"
#include "GlobalNamespace/ISpriteAsyncLoader.hpp"
#include "GlobalNamespace/StandardLevelInfoSaveData.hpp"
#include "GlobalNamespace/StandardLevelDetailView.hpp"
#include <regex>
#include "GlobalNamespace/MediaAsyncLoader.hpp"
using namespace GlobalNamespace;
using namespace UnityEngine;
using namespace System::Threading::Tasks;
using namespace System::IO;
using namespace System::Threading;
static ModInfo modInfo; // Stores the ID and version of our mod, and is sent to the modloader upon startup


#define coro(coroutine) GlobalNamespace::SharedCoroutineStarter::get_instance()->StartCoroutine(custom_types::Helpers::CoroutineHelper::New(coroutine))

// Loads the config from disk using our modInfo, then returns it for use
// other config tools such as config-utils don't use this config, so it can be removed if those are in use
Configuration& getConfig() {
    static Configuration config(modInfo);
    return config;
}

// Returns a logger, useful for printing debug messages
Logger& getLoggerOld() {
    static auto* logger = new Logger(modInfo, LoggerOptions(false, true));
    return *logger;
}

// Returns a logger, useful for printing debug messages
Paper::ConstLoggerContext<14UL> getLogger() {
    static auto fastContext = Paper::Logger::WithContext<MOD_ID>();
    return fastContext;
}

// Called at the early stages of game loading
extern "C" void setup(ModInfo& info) {
    info.id = MOD_ID;
    info.version = VERSION;
    modInfo = info;
	
    getConfig().Load();
    getLoggerOld().info("Completed setup!");
}

static int MAX_CACHED_COVERS = 7;


// This is needed to avoid race conditions when clearing the cache
void ClearUnusedCovers() {
    static std::atomic<bool> isRunning = false;

    if (isRunning) {
        return;
    }

    QuestUI::MainThreadScheduler::Schedule([]{
        // Extra check just in case
        if (isRunning) return;
        std::lock_guard<std::mutex> lock(coverCacheInvalidatorMutex);
        isRunning = true;
        for(int i = coverCacheInvalidator.size() - MAX_CACHED_COVERS; i-- > 0;) {
            auto songToInvalidate = coverCacheInvalidator[i];
            
            // Skip selected level
            DEBUG("Comparing {} to {}", lastSelectedLevel, songToInvalidate.levelId);
            if(lastSelectedLevel == songToInvalidate.levelId) {
                DEBUG("Skipping selected level {}", lastSelectedLevel);
                continue;
            }


            auto coverCacheEntry = coverCache.at(songToInvalidate.levelId);

            if(
                coverCacheEntry != nullptr &&
                coverCacheEntry->m_CachedPtr.m_value != nullptr
            ) {
                DEBUG("Clearing cover for {}", songToInvalidate.levelId);
                auto * texture = songToInvalidate.cover->get_texture();
                if (texture != nullptr && texture->m_CachedPtr.m_value != nullptr) {
                    DEBUG("Destroying texture for {}", songToInvalidate.levelId);
                    Object::Destroy(texture);
                }
                Object::Destroy(songToInvalidate.cover);
                
            } else {
                DEBUG("Cover for {} is null", songToInvalidate.levelId);
            }

            coverCacheInvalidator.erase(coverCacheInvalidator.begin()+i);
            coverCache.erase(songToInvalidate.levelId);
        }     
        isRunning = false;
    });
}


MAKE_HOOK_MATCH(StandardLevelDetailView_SetContent, &StandardLevelDetailView::SetContent, void, StandardLevelDetailView* self, IBeatmapLevel* level, BeatmapDifficulty defaultDifficulty, BeatmapCharacteristicSO* defaultBeatmapCharacteristic, PlayerData* playerData) {
    // Prefix
    // fix
    StandardLevelDetailView_SetContent(self, level, defaultDifficulty, defaultBeatmapCharacteristic, playerData);
    
    lastSelectedLevel = std::string(level->i_IPreviewBeatmapLevel()->get_levelID());
};

void PrintStruct() {

    

}

MAKE_HOOK_MATCH(CustomPreviewBeatmapLevel_GetCoverImageAsync, &CustomPreviewBeatmapLevel::GetCoverImageAsync, Task_1<Sprite *>*, CustomPreviewBeatmapLevel* self, System::Threading::CancellationToken cancellationToken) {
    if (System::String::IsNullOrEmpty(self->standardLevelInfoSaveData->coverImageFilename)) {
        return Task_1<Sprite *>::FromResult(self->defaultCoverImage);
    }

    auto levelId = self->get_levelID();

    // If custom cache has cover
    if (coverCache.contains(levelId)) {
        std::lock_guard<std::mutex> lock(coverCacheInvalidatorMutex);
        int cachedIndex = -1;
            
        // "Refresh" the cover in the cache LIFO
        for (auto i=0; i< coverCacheInvalidator.size(); i++) {
            if (coverCacheInvalidator[i].levelId == std::string(levelId)) {
                cachedIndex = i;
                break;
            }
        }

        // Move to top
        if (cachedIndex != 1 && cachedIndex + 1 != coverCacheInvalidator.size()) {
            auto item = coverCacheInvalidator[cachedIndex];
            coverCacheInvalidator.erase(coverCacheInvalidator.begin()+cachedIndex);
            coverCacheInvalidator.push_back(item);
        }

        DEBUG("Custom cache fired");
        return Task_1<Sprite *>::FromResult(coverCache.at(levelId));
    }

    
    StringW path = Path::Combine(self->customLevelPath, self->standardLevelInfoSaveData->coverImageFilename);

    if(!File::Exists(path)) {
        DEBUG("File does not exist");
        return Task_1<Sprite *>::FromResult(self->defaultCoverImage);
    }

    if (cancellationToken.get_IsCancellationRequested()) {
        return nullptr;
    }
    
    using Task = Task_1<UnityEngine::Sprite*>*;
    using Action = System::Func_2<Task, UnityEngine::Sprite*>*;

    auto middleware = custom_types::MakeDelegate<Action>(classof(Action), static_cast<std::function<Sprite* (Task)>>([levelId](Task resultTask) {
        DEBUG("Middleware fired");
        bool cancelled = resultTask->get_IsCanceled();
        if (cancelled) {
            return (Sprite*)nullptr;
        }
        UnityEngine::Sprite* cover = resultTask->get_ResultOnSuccess();
        if (cover != nullptr && cover->m_CachedPtr.m_value != nullptr) {
            
            std::lock_guard<std::mutex> lock(coverCacheInvalidatorMutex);

            DEBUG("Level id {}", std::string(levelId));
            
            // Maybe
            if (coverCache.contains(levelId)) {
                WARNING("Cover is already in the cover cache");
            }

            coverCache.emplace(levelId, cover);
            coverCacheInvalidator.push_back({
                std::string(levelId), cover
            });

            // Call clear unused covers
            ClearUnusedCovers();

            return cover;
        } else {
            DEBUG("Cover is null, returning nullptr");
            return (Sprite*)nullptr;
        }
    }));

    DEBUG("Running middleware");
    auto lol = MediaAsyncLoader::LoadSpriteAsync(path, cancellationToken);
    static auto internalLogger = ::Logger::get().WithContext("::Task_1::ContinueWith");
    static auto* method = ::il2cpp_utils::FindMethodUnsafe(lol, "ContinueWith", 1);
    static auto* genericMethod = THROW_UNLESS(::il2cpp_utils::MakeGenericMethod(method, std::vector<Il2CppClass*>{::il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_class<Sprite*>::get()}));
    return ::il2cpp_utils::RunMethodRethrow<::Task_1<Sprite*>*, false>(lol, genericMethod, middleware);
};



// Called later on in the game loading - a good time to install function hooks
extern "C" void load() {
    il2cpp_functions::Init();

    getLoggerOld().info("Installing hooks...");

    INSTALL_HOOK(getLoggerOld(), StandardLevelDetailView_SetContent);
    INSTALL_HOOK(getLoggerOld(), CustomPreviewBeatmapLevel_GetCoverImageAsync);

    getLoggerOld().info("Installed all hooks!");
}

