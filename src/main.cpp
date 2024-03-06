#include "main.hpp"
#include "GlobalNamespace/PlayerData.hpp"
#include "bsml/shared/BSML/MainThreadScheduler.hpp"
#include "System/Threading/Tasks/TaskCanceledException.hpp"
#include "GlobalNamespace/IBeatmapLevelPack.hpp"
#include "GlobalNamespace/IBeatmapLevel.hpp"
#include "GlobalNamespace/IBeatmapLevelData.hpp"
#include "GlobalNamespace/IPreviewBeatmapLevel.hpp"
#include "System/Action.hpp"
#include "System/Func_1.hpp"
#include "System/Func_2.hpp"
#include "System/Action_1.hpp"
#include "System/Threading/Tasks/Task_1.hpp"
#include "System/IO/Path.hpp"
#include "System/IO/File.hpp"
#include "UnityEngine/Object.hpp"
#include "UnityEngine/Resources.hpp"
#include "custom-types/shared/delegate.hpp"
#include "UnityEngine/Texture2D.hpp"
#include "UnityEngine/HideFlags.hpp"
#include "GlobalNamespace/IDifficultyBeatmap.hpp"
#include "GlobalNamespace/ISpriteAsyncLoader.hpp"
#include "GlobalNamespace/StandardLevelInfoSaveData.hpp"
#include "GlobalNamespace/StandardLevelDetailView.hpp"
#include "GlobalNamespace/CustomPreviewBeatmapLevel.hpp"
#include <regex>
#include "GlobalNamespace/MediaAsyncLoader.hpp"
using namespace GlobalNamespace;
using namespace UnityEngine;
using namespace System::Threading::Tasks;
using namespace System::IO;
using namespace System::Threading;
inline modloader::ModInfo modInfo = {MOD_ID, VERSION, 0}; // Stores the ID and version of our mod, and is sent to the modloader upon startup

#define coro(coroutine) BSML::SharedCoroutineStarter::get_instance()->StartCoroutine(custom_types::Helpers::CoroutineHelper::New(coroutine))

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
extern "C" __attribute__((visibility("default"))) void setup(CModInfo& info) {
    info.id = MOD_ID;
    info.version = VERSION;
    info.version_long = 0;
    modInfo.assign(info);
	
    getConfig().Load();
    getLoggerOld().info("Completed setup!");
}

static int MAX_CACHED_COVERS = 3;

// This is needed to avoid race conditions when clearing the cache
void ClearUnusedCovers() {
    static std::atomic<bool> isRunning = false;

    if (isRunning) {
        return;
    }

    BSML::MainThreadScheduler::Schedule([]{
        // Extra check just in case
        if (isRunning) return;
        std::lock_guard<std::mutex> lock(coverCacheInvalidatorMutex);

        isRunning = true;

        // If we have less or equal MAX_CACHED_COVERS covers cached, don't clear anything 
        if (coverCacheInvalidator.size() < MAX_CACHED_COVERS) {
            isRunning = false;
            return;
        }

        for(int i = coverCacheInvalidator.size() - MAX_CACHED_COVERS; i-- > 0;) {
            auto songToInvalidate = coverCacheInvalidator[i];
           
            if(lastSelectedLevel == songToInvalidate.levelId) {
                continue;
            }

            if (!coverCache.contains(songToInvalidate.levelId)) {
                WARNING("Cover for {} not found in cache", songToInvalidate.levelId);
                continue;
            }

            auto coverCacheEntry = coverCache.at(songToInvalidate.levelId);

            if(coverCacheEntry) {
                auto texture = coverCacheEntry->get_texture();
                Object::DestroyImmediate(coverCacheEntry);
                if (texture) {
                    Object::DestroyImmediate(texture);
                }
                
            } else {
                WARNING("Cover for {} is null", songToInvalidate.levelId);
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
    
    lastSelectedLevel = std::string(level->i___GlobalNamespace__IPreviewBeatmapLevel()->get_levelID());
};


MAKE_HOOK_MATCH(CustomPreviewBeatmapLevel_GetCoverImageAsync, &CustomPreviewBeatmapLevel::GetCoverImageAsync, Task_1<UnityW<Sprite>>*, CustomPreviewBeatmapLevel* self, System::Threading::CancellationToken cancellationToken) {
    try {
        DEBUG("GetCoverImageAsync hook called");
    if (System::String::IsNullOrEmpty(self->get_standardLevelInfoSaveData()->get_coverImageFilename())) {
        return Task_1<UnityW<Sprite>>::FromResult(self->get_defaultCoverImage());
    }

    auto levelId = self->get_levelID();

    DEBUG("Level ID: {}", to_utf8(csstrtostr(levelId)).c_str());
    // If custom cache has cover
    if (coverCache.contains(levelId)) {
        std::lock_guard<std::mutex> lock(coverCacheInvalidatorMutex);
        int cachedIndex = -1;
            
        // "Refresh" the cover in the cache LIFO
        for (auto i = 0; i < coverCacheInvalidator.size(); i++) {
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

        return Task_1<UnityW<Sprite>>::FromResult(coverCache.at(levelId));
    }

    
    StringW path = Path::Combine(self->get_customLevelPath(), self->get_standardLevelInfoSaveData()->get_coverImageFilename());

    if(!File::Exists(path)) {
        DEBUG("File does not exist");
        return Task_1<Sprite *>::FromResult(self->get_defaultCoverImage());
    }

    if (cancellationToken.get_IsCancellationRequested()) {
        return nullptr;
    }
    
    using Task = Task_1<UnityEngine::Sprite*>*;
    using Action = System::Func_2<Task, UnityEngine::Sprite*>*;

    auto middleware = custom_types::MakeDelegate<Action>(classof(Action), static_cast<std::function<UnityW<Sprite> (Task)>>([levelId](Task resultTask) {
        UnityW<UnityEngine::Sprite> cover = resultTask->get_ResultOnSuccess();
        try {
            if (cover != nullptr && cover->___m_CachedPtr != nullptr) {
            {
                std::lock_guard<std::mutex> lock(coverCacheInvalidatorMutex);

                // If the cover is already in the cache, return it and remove the old entry
                if (coverCache.contains(levelId)) {
                    // Find the old entry and revalidate it
                    int cachedIndex = -1;
                        
                    // "Refresh" the cover in the cache LIFO
                    for (auto i = 0; i < coverCacheInvalidator.size(); i++) {
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

                    WARNING("Cover is already in the cover cache");


                    BSML::MainThreadScheduler::Schedule([cover]{
                        UnityEngine::Object::DestroyImmediate(cover);
                    });
                    

                    // Get the cached entry and return
                    auto coverCacheEntry = coverCache.at(levelId);
                    return coverCacheEntry;
                     
                }

                coverCache.emplace(levelId, cover);

                coverCacheInvalidator.push_back({
                    std::string(levelId), cover
                });
            }
            
            // Call clear unused covers
            ClearUnusedCovers();
            
            return cover;
        } else {
            return (UnityW<Sprite>)nullptr;
        }
        } catch (...) {
            getLoggerOld().error("An error occurred in GetCoverImageAsync middleware");
            return (UnityW<Sprite>)nullptr;
        }
        
    }));

    // WARNING if you don't use get_None() it will leak memory every time it gets cancelled
    auto lol = MediaAsyncLoader::LoadSpriteAsync(path, CancellationToken::get_None());
    static auto internalLogger = ::Logger::get().WithContext("::Task_1::ContinueWith");
    static auto* method = ::il2cpp_utils::FindMethodUnsafe(lol, "ContinueWith", 1);
    static auto* genericMethod = THROW_UNLESS(::il2cpp_utils::MakeGenericMethod(method, std::vector<Il2CppClass*>{::il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_class<Sprite*>::get()}));
    return ::il2cpp_utils::RunMethodRethrow<::Task_1<UnityW<Sprite>>*, false>(lol, genericMethod, middleware);
    } catch(...) {
        getLoggerOld().error("An error occurred in GetCoverImageAsync");
        return Task_1<UnityW<Sprite>>::FromResult(self->get_defaultCoverImage());
    }
    
};



// Called later on in the game loading - a good time to install function hooks
extern "C" __attribute__((visibility("default"))) void late_load() {
    il2cpp_functions::Init();

    getLoggerOld().info("Installing hooks...");

    INSTALL_HOOK(getLoggerOld(), StandardLevelDetailView_SetContent);
    INSTALL_HOOK(getLoggerOld(), CustomPreviewBeatmapLevel_GetCoverImageAsync);

    getLoggerOld().info("Installed all hooks!");
}

