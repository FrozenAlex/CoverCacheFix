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

//// TASK UTILS
template<typename Ret, typename T>
requires(std::is_invocable_r_v<Ret, T>)
void task_func(System::Threading::Tasks::Task_1<Ret>* task, T func) {
    task->TrySetResult(std::invoke(func));
}

template<typename Ret, typename T>
requires(std::is_invocable_r_v<Ret, T>)
void task_cancel_func(System::Threading::Tasks::Task_1<Ret>* task, T func, System::Threading::CancellationToken cancelToken) {
    auto value = std::invoke(func);
    if (!cancelToken.IsCancellationRequested) {
        task->TrySetResult(func);
    } else {
        task->TrySetCanceled(cancelToken);
    }
}

template<typename Ret, typename T>
requires(!std::is_same_v<Ret, void> && std::is_invocable_r_v<Ret, T>)
System::Threading::Tasks::Task_1<Ret>* StartTask(T func) {
    auto t = System::Threading::Tasks::Task_1<Ret>::New_ctor();
    il2cpp_utils::il2cpp_aware_thread(&task_func<Ret, T>, t, func).detach();
    return t;
}

template<typename Ret, typename T>
requires(!std::is_same_v<Ret, void> && std::is_invocable_r_v<Ret, T>)
System::Threading::Tasks::Task_1<Ret>* StartTask(T func, System::Threading::CancellationToken cancelToken) {
    auto t = System::Threading::Tasks::Task_1<Ret>::New_ctor();
    il2cpp_utils::il2cpp_aware_thread(&task_cancel_func<Ret, T>, t, func, cancelToken).detach();
    return t;
}

//// TASK UTILS


static int MAX_CACHED_COVERS = 50;


/**
 * @brief Remove a cover from the cache by levelId (does not destroy the cover image)
 * 
 * @param levelId 
 * @return true 
 * @return false 
 */
bool RemoveCoverFromCache(std::string levelId) {
    int cachedIndex = -1;
    // "Refresh" the cover in the cache LIFO
    for (auto i = 0; i < coverCacheInvalidator.size(); i++) {
        if (coverCacheInvalidator[i].levelId == std::string(levelId)) {
            cachedIndex = i;
            break;
        }
    }

    if (cachedIndex == -1) return false;
 

    // Remove from invalidator
    if (cachedIndex != -1 && cachedIndex + 1 != coverCacheInvalidator.size()) {
        auto item = coverCacheInvalidator[cachedIndex];
        coverCacheInvalidator.erase(coverCacheInvalidator.begin()+cachedIndex);
    }
    coverCache.erase(levelId);

    return true;
    
}

bool RefreshCoverCache(std::string levelId) {
    int cachedIndex = -1;

    // "Refresh" the cover in the cache LIFO
    for (auto i = 0; i < coverCacheInvalidator.size(); i++) {
        if (coverCacheInvalidator[i].levelId == std::string(levelId)) {
            cachedIndex = i;
            break;
        }
    }

    // Move to top
    if (cachedIndex != -1 && cachedIndex + 1 != coverCacheInvalidator.size()) {
        auto item = coverCacheInvalidator[cachedIndex];
        coverCacheInvalidator.erase(coverCacheInvalidator.begin()+cachedIndex);
        coverCacheInvalidator.push_back(item);
        return true;
    }
    return false;
}

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
                Object::Destroy(coverCacheEntry);
                if (texture) {
                    Object::Destroy(texture);
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
        // DEBUG("GetCoverImageAsync hook called");
    if (System::String::IsNullOrEmpty(self->get_standardLevelInfoSaveData()->get_coverImageFilename())) {
        return Task_1<UnityW<Sprite>>::FromResult(self->get_defaultCoverImage());
    }

    auto levelId = self->get_levelID();

    // DEBUG("Level ID: {}", to_utf8(csstrtostr(levelId)).c_str());

    {
        std::lock_guard<std::mutex> lock(coverCacheInvalidatorMutex);
        // If custom cache has cover that is not null, return it
        if (coverCache.contains(levelId)) {
            // If the cover is already in the cache, return it and remove the old entry
            if (coverCache.at(levelId)) {
                RefreshCoverCache(levelId);

                return Task_1<UnityW<Sprite>>::FromResult(coverCache.at(levelId));
            } else {
                // If the cover is null, remove it from the cache
                RemoveCoverFromCache(levelId);
            }
        }
    }
    

    
    StringW path = Path::Combine(self->get_customLevelPath(), self->get_standardLevelInfoSaveData()->get_coverImageFilename());

    if(!File::Exists(path)) {
        DEBUG("File does not exist");
        return Task_1<Sprite *>::FromResult(self->get_defaultCoverImage());
    }

    if (cancellationToken.get_IsCancellationRequested()) {
        return nullptr;
    }
    
  

    // WARNING if you don't use get_None() it will leak memory every time it gets cancelled
    // auto lol = MediaAsyncLoader::LoadSpriteAsync(path, CancellationToken::get_None());
    // static auto internalLogger = ::Logger::get().WithContext("::Task_1::ContinueWith");
    // static auto* method = ::il2cpp_utils::FindMethodUnsafe(lol, "ContinueWith", 1);
    // static auto* genericMethod = THROW_UNLESS(::il2cpp_utils::MakeGenericMethod(method, std::vector<Il2CppClass*>{::il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_class<Sprite*>::get()}));
    // return ::il2cpp_utils::RunMethodRethrow<::Task_1<UnityW<Sprite>>*, false>(lol, genericMethod, middleware);
        Task_1<UnityW<UnityEngine::Sprite>>* loadSpriteTask = MediaAsyncLoader::LoadSpriteAsync(path, CancellationToken::get_None());

        return StartTask<UnityW<Sprite>>([=]{
            // Wait for the task to complete
            loadSpriteTask->Wait();

            if (loadSpriteTask->get_IsCompleted()) {
                auto cover = loadSpriteTask->get_ResultOnSuccess();

                if (cover) {
                    
                    std::lock_guard<std::mutex> lock(coverCacheInvalidatorMutex);

                    // If the cover is already in the cache, return it and remove the old entry
                    if (coverCache.contains(levelId) && coverCache.at(levelId)) {

                        RefreshCoverCache(levelId);
                        
                        WARNING("Cover is already in the cover cache");
                        
                        // Destroy the new cover
                        BSML::MainThreadScheduler::Schedule([cover]{
                            UnityEngine::Object::Destroy(cover);
                        });
                        
                        // Get the cached entry and return
                        auto coverCacheEntry = coverCache.at(levelId);
                        
                        return coverCacheEntry;
                    } else {
                        // Add to cache
                        coverCache.emplace(levelId, cover);

                        coverCacheInvalidator.push_back({
                            std::string(levelId), cover
                        });

                        // Call clear unused covers
                        ClearUnusedCovers();

                        return cover;
                    }
                     
                } else {
                    return self->get_defaultCoverImage();
                }


            } else {
                DEBUG("Task is not completed");
                return self->get_defaultCoverImage();
            }
        }, CancellationToken::get_None());

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

