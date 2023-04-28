#include "main.hpp"
#include "GlobalNamespace/PlayerData.hpp"
#include "questui/shared/CustomTypes/Components/MainThreadScheduler.hpp"
#include "questui/shared/BeatSaberUI.hpp"
#include "System/Threading/Tasks/TaskCanceledException.hpp"
#include "GlobalNamespace/IBeatmapLevelPack.hpp"
#include "GlobalNamespace/IBeatmapLevel.hpp"
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
#include "GlobalNamespace/ISpriteAsyncLoader.hpp"
#include "GlobalNamespace/StandardLevelInfoSaveData.hpp"
#include "GlobalNamespace/StandardLevelDetailView.hpp"
#include <regex>
using namespace GlobalNamespace;
using namespace UnityEngine;
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




MAKE_HOOK_MATCH(StandardLevelDetailView_SetContent, &StandardLevelDetailView::SetContent, void, StandardLevelDetailView* self, IBeatmapLevel* level, BeatmapDifficulty defaultDifficulty, BeatmapCharacteristicSO* defaultBeatmapCharacteristic, PlayerData* playerData) {
    // Prefix
    // fix
    StandardLevelDetailView_SetContent(self, level, defaultDifficulty, defaultBeatmapCharacteristic, playerData);
    // postfix
    lastSelectedLevel = level;
};

MAKE_HOOK_MATCH(CustomPreviewBeatmapLevel_GetCoverImageAsync, &CustomPreviewBeatmapLevel::GetCoverImageAsync, System::Threading::Tasks::Task_1<UnityEngine::Sprite *>*, CustomPreviewBeatmapLevel* self, System::Threading::CancellationToken cancellationToken) {
    static int MAX_CACHED_COVERS = 20;
    if (self->coverImage != nullptr && self->coverImage->m_CachedPtr.m_value != nullptr) {
        int cachedIndex = -1;
        // "Refresh" the cover in the cache LIFO
        for (auto i=0; i< coverCacheInvalidator.size(); i--) {
            auto level = reinterpret_cast<CustomPreviewBeatmapLevel*>(coverCacheInvalidator[i].level);
            if (level == self) {
                cachedIndex = i;
                break;
            } 
        }

        // Move to top
        if (cachedIndex != 1 && cachedIndex + 1 != coverCacheInvalidator.size()) {
            coverCacheInvalidator.push_back(coverCacheInvalidator[cachedIndex]);
            coverCacheInvalidator.erase(coverCacheInvalidator.begin()+cachedIndex);
        }

        DEBUG("Cached image");
        return System::Threading::Tasks::Task_1<UnityEngine::Sprite *>::FromResult(self->coverImage);
    }

    if (System::String::IsNullOrEmpty(self->standardLevelInfoSaveData->coverImageFilename)) {
        DEBUG("Default image");
        return System::Threading::Tasks::Task_1<UnityEngine::Sprite *>::FromResult(self->defaultCoverImage);
    }

    StringW path = System::IO::Path::Combine(self->customLevelPath, self->standardLevelInfoSaveData->coverImageFilename);

    if(!System::IO::File::Exists(path)) {
        DEBUG("File does not exist");
        return System::Threading::Tasks::Task_1<UnityEngine::Sprite *>::FromResult(self->defaultCoverImage);
    }

    if (cancellationToken.get_IsCancellationRequested()) {
        DEBUG("Fix tis");
        // return System::Threading::Tasks::Task_1<UnityEngine::Sprite *>::FromException(System::Exception::New_ctor("Cancelled"));
    }
    
    using Task = System::Threading::Tasks::Task_1<UnityEngine::Sprite*>*;
    using Action = System::Func_2<Task, UnityEngine::Sprite*>*;

    auto middleware = custom_types::MakeDelegate<Action>(classof(Action), static_cast<std::function<UnityEngine::Sprite* (Task)>>([self](Task resultTask) {
        bool cancelled = resultTask->get_IsCanceled();
        if (cancelled) {
            DEBUG("Task cancelled");
            return nullptr;
        }
        UnityEngine::Sprite* cover = resultTask->get_ResultOnSuccess();
        if (cover != nullptr && cover->m_CachedPtr.m_value != nullptr) {
            self->coverImage = cover;
            coverCacheInvalidator.push_back({
                reinterpret_cast<GlobalNamespace::IBeatmapLevel*>(self), self->coverImage
            });

            QuestUI::MainThreadScheduler::Schedule([]{
                for(int i = coverCacheInvalidator.size() - MAX_CACHED_COVERS; i-- > 0;) {

                auto songToInvalidate = coverCacheInvalidator[i];
                // Skip selected level
                if(lastSelectedLevel == songToInvalidate.level)
            		continue;
                coverCacheInvalidator.erase(coverCacheInvalidator.begin()+i);
                if(songToInvalidate.level != nullptr && songToInvalidate.cover != nullptr && songToInvalidate.cover->m_CachedPtr.m_value != nullptr) {
                    auto song = reinterpret_cast<CustomPreviewBeatmapLevel*>(songToInvalidate.level);
                    song->coverImage = nullptr;
                    DEBUG("DeletingCover {}", std::string(song->songName) );
                    if (songToInvalidate.cover != nullptr && songToInvalidate.cover->get_texture() != nullptr) {
                    Object::DestroyImmediate(songToInvalidate.cover->get_texture());    
                    }
                    if (songToInvalidate.cover != nullptr) {
                        Object::DestroyImmediate(songToInvalidate.cover);
                    }
                    
                }
            }     
            });
        } else {
            DEBUG("No cover found");
        }
        return nullptr;
    }));
    // self->get_spriteAsyncLoader()->LoadSpriteAsync(path, cancellationToken);


// System.Threading.Tasks.Task<TNewResult> ContinueWith(System.Func<System.Threading.Tasks.Task<UnityEngine.Sprite>,TNewResult> continuationFunction);
// System.Threading.Tasks.Task<TNewResult> ContinueWith(System.Func<System.Threading.Tasks.Task<UnityEngine.Sprite>,TNewResult> continuationFunction, System.Threading.Tasks.TaskScheduler scheduler, System.Threading.CancellationToken cancellationToken, System.Threading.Tasks.TaskContinuationOptions continuationOptions, out/ref System.Threading.StackCrawlMark& stackMark);


    auto lol = self->get_spriteAsyncLoader()->LoadSpriteAsync(path, System::Threading::CancellationToken::get_None());

    static auto ___internal__logger = ::Logger::get().WithContext("::System::Threading::Tasks::Task_1::ContinueWith");
    // static auto* ___internal__method = THROW_UNLESS((::il2cpp_utils::FindMethod(lol, "ContinueWith", std::vector<Il2CppClass*>{::il2cpp_utils::il2cpp_type_check::il2cpp_gen_struct_no_arg_class<UnityEngine::Sprite*>::get()}, ::std::vector<const Il2CppType*>{::il2cpp_utils::ExtractType(middleware)})));
    static auto* ___internal__method = ::il2cpp_utils::FindMethodUnsafe(lol, "ContinueWith", 1);
    static auto* ___generic__method = THROW_UNLESS(::il2cpp_utils::MakeGenericMethod(___internal__method, std::vector<Il2CppClass*>{::il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_class<UnityEngine::Sprite*>::get()}));
    return ::il2cpp_utils::RunMethodRethrow<::System::Threading::Tasks::Task_1<UnityEngine::Sprite*>*, false>(lol, ___generic__method, middleware);



    // return lol->ContinueWith(action);
    // return CustomPreviewBeatmapLevel_GetCoverImageAsync(self, cancellationToken);
    // postfix
   
};



// Called later on in the game loading - a good time to install function hooks
extern "C" void load() {
    il2cpp_functions::Init();

    getLoggerOld().info("Installing hooks...");

    INSTALL_HOOK(getLoggerOld(), StandardLevelDetailView_SetContent);
    INSTALL_HOOK(getLoggerOld(), CustomPreviewBeatmapLevel_GetCoverImageAsync);

    getLoggerOld().info("Installed all hooks!");
}

