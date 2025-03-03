// Copyright (c) 2017 Computer Vision Center (CVC) at the Universitat Autonoma
// de Barcelona (UAB).
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "Carla.h"
#include "Carla/Server/CarlaServer.h"
#include "Carla/Server/CarlaServerResponse.h"
#include "Carla/Traffic/TrafficLightGroup.h"
#include "EngineUtils.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"

#include "Carla/OpenDrive/OpenDrive.h"
#include "Carla/Util/DebugShapeDrawer.h"
#include "Carla/Util/NavigationMesh.h"
#include "Carla/Util/RayTracer.h"
#include "Carla/Vehicle/CarlaWheeledVehicle.h"
#include "Carla/Sensor/CustomV2XSensor.h"
#include "Carla/Walker/WalkerController.h"
#include "Carla/Walker/WalkerBase.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Carla/Game/Tagger.h"
#include "Carla/Game/CarlaStatics.h"
#include "Carla/Vehicle/MovementComponents/CarSimManagerComponent.h"
#include "Carla/Vehicle/MovementComponents/ChronoMovementComponent.h"
#include "Carla/Lights/CarlaLightSubsystem.h"
#include "Carla/Actor/ActorData.h"
#include "CarlaServerResponse.h"
#include "Carla/Util/BoundingBoxCalculator.h"
#include "Misc/FileHelper.h"

#include <compiler/disable-ue4-macros.h>
#include <carla/Functional.h>
#include <carla/multigpu/router.h>
#include <carla/Version.h>
#include <carla/rpc/AckermannControllerSettings.h>
#include <carla/rpc/Actor.h>
#include <carla/rpc/ActorDefinition.h>
#include <carla/rpc/ActorDescription.h>
#include <carla/rpc/BoneTransformDataIn.h>
#include <carla/rpc/Command.h>
#include <carla/rpc/CommandResponse.h>
#include <carla/rpc/DebugShape.h>
#include <carla/rpc/EnvironmentObject.h>
#include <carla/rpc/EpisodeInfo.h>
#include <carla/rpc/EpisodeSettings.h>
#include <carla/rpc/LabelledPoint.h>
#include <carla/rpc/LightState.h>
#include <carla/rpc/MapInfo.h>
#include <carla/rpc/MapLayer.h>
#include <carla/rpc/Response.h>
#include <carla/rpc/Server.h>
#include <carla/rpc/String.h>
#include <carla/rpc/Transform.h>
#include <carla/rpc/Vector2D.h>
#include <carla/rpc/Vector3D.h>
#include <carla/rpc/VehicleDoor.h>
#include <carla/rpc/VehicleAckermannControl.h>
#include <carla/rpc/VehicleControl.h>
#include <carla/rpc/VehiclePhysicsControl.h>
#include <carla/rpc/VehicleLightState.h>
#include <carla/rpc/VehicleLightStateList.h>
#include <carla/rpc/VehicleTelemetryData.h>
#include <carla/rpc/WalkerBoneControlIn.h>
#include <carla/rpc/WalkerBoneControlOut.h>
#include <carla/rpc/WalkerControl.h>
#include <carla/rpc/VehicleWheels.h>
#include <carla/rpc/WeatherParameters.h>
#include <carla/streaming/detail/Types.h>
#include <carla/rpc/Texture.h>
#include <carla/rpc/MaterialParameter.h>
#include <compiler/enable-ue4-macros.h>

#include <vector>
#include <atomic>
#include <map>
#include <tuple>

template <typename T>
using R = carla::rpc::Response<T>;

// =============================================================================
// -- 静态局部函数 --------------------------------------------------------------
// =============================================================================

// 通过数组构建向量
template <typename T, typename Other>
static std::vector<T> MakeVectorFromTArray(const TArray<Other> &Array)
{
  return {Array.GetData(), Array.GetData() + Array.Num()};
}

// =============================================================================
// -- FCarlaServer::FPimpl -----------------------------------------------
// =============================================================================

class FCarlaServer::FPimpl
{
public:

  FPimpl(uint16_t RPCPort, uint16_t StreamingPort, uint16_t SecondaryPort)
    : Server(RPCPort),
      StreamingServer(StreamingPort),
      BroadcastStream(StreamingServer.MakeStream())
  {
    // 我们需要从路由中创建指向 carla::multigpu::Router 类型的智能指针 shared_ptr，以便一些处理程序能够存活
    // 使用make_shared函数可以减少内存分配的次数，因为它会在一次内存分配中同时分配智能指针对象和指向的对象。
    // std::make_shared函数会自动分配内存并构造对象，因此不需要手动调用new操作符
    SecondaryServer = std::make_shared<carla::multigpu::Router>(SecondaryPort); // 从服务器
    SecondaryServer->SetCallbacks();  // 设置从服务器的回调函数
    BindActions();
  }

  std::shared_ptr<carla::multigpu::Router> GetSecondaryServer() {
    return SecondaryServer;
  }

  /// 仿真中所有活动的交通管理器对 < port, ip > 的映射
  std::map<uint16_t, std::string> TrafficManagerInfo;

  carla::rpc::Server Server;

  carla::streaming::Server StreamingServer;

  carla::streaming::Stream BroadcastStream;

  std::shared_ptr<carla::multigpu::Router> SecondaryServer;

  UCarlaEpisode *Episode = nullptr;

  std::atomic_size_t TickCuesReceived { 0u };  // 收到的节拍提示

private:

  void BindActions();
};

// =============================================================================
// -- 定义辅助宏 ----------------------------------------------------------------
// =============================================================================
// 检查当前是否在游戏线程中执行，仅在编辑器模式下有效
#if WITH_EDITOR
#  define CARLA_ENSURE_GAME_THREAD() check(IsInGameThread());// 在编辑器中，确保代码在游戏线程中执行
#else
#  define CARLA_ENSURE_GAME_THREAD()// 在非编辑器模式下，此宏不做任何事情
#endif // WITH_EDITOR
// 定义一个宏，用于记录错误消息并返回一个ResponseError对象
#define RESPOND_ERROR(str) {                                              \
    UE_LOG(LogCarlaServer, Log, TEXT("Responding error: %s"), TEXT(str)); \
    return carla::rpc::ResponseError(str); }

// 定义一个宏，用于记录由FString表示的错误消息并返回一个ResponseError对象
#define RESPOND_ERROR_FSTRING(fstr) {                                 \
    UE_LOG(LogCarlaServer, Log, TEXT("Responding error: %s"), *fstr); \
    return carla::rpc::ResponseError(carla::rpc::FromFString(fstr)); }
// 定义一个宏，用于确保有一个有效的CARLA Episode对象，并检查是否在游戏线程中
#define REQUIRE_CARLA_EPISODE() \
    CARLA_ENSURE_GAME_THREAD();   \
    if (Episode == nullptr) { RESPOND_ERROR("episode not ready"); }
// 定义一个函数，用于构造并返回一个包含函数名、错误消息和额外信息的ResponseError对象
carla::rpc::ResponseError RespondError(
    const FString& FuncName,
    const FString& ErrorMessage,
    const FString& ExtraInfo = "")
{
  FString TotalMessage = "Responding error from function " + FuncName + ": " +
      ErrorMessage + ". " + ExtraInfo;// 拼接完整的错误消息
  UE_LOG(LogCarlaServer, Log, TEXT("%s"), *TotalMessage);// 记录完整错误消息到日志
  return carla::rpc::ResponseError(carla::rpc::FromFString(TotalMessage)); // 返回一个包含完整错误消息的ResponseError对象
}
// 定义一个重载函数，用于处理ECarlaServerResponse枚举值作为错误代码的情况
carla::rpc::ResponseError RespondError(
    const FString& FuncName,
    const ECarlaServerResponse& Error,
    const FString& ExtraInfo = "")
{
  return RespondError(FuncName, CarlaGetStringError(Error), ExtraInfo);
}

//将自定义函数绑定到 CARLA 模拟器中的 RPC 服务器，以响应来自模拟器的请求或事件
//通过指定同步或异步模式，控制这些函数是如何被调用的，这对于处理实时数据或模拟中的事件非常重要
class ServerBinder
{
public:

  constexpr ServerBinder(const char *name, carla::rpc::Server &srv, bool sync)
    : _name(name),
      _server(srv),
      _sync(sync) {}

  template <typename FuncT>
  auto operator<<(FuncT func)
  {
    if (_sync)
    {
      _server.BindSync(_name, func);
    }
    else
    {
      _server.BindAsync(_name, func);
    }
    return func;
  }

private:

  const char *_name;

  carla::rpc::Server &_server;

  bool _sync;
};

#define BIND_SYNC(name)   auto name = ServerBinder(# name, Server, true)
#define BIND_ASYNC(name)  auto name = ServerBinder(# name, Server, false)

// =============================================================================
// -- 绑定操作 -------------------------------------------------------------
// =============================================================================

void FCarlaServer::FPimpl::BindActions()
{
  namespace cr = carla::rpc;
  namespace cg = carla::geom;

  /// 寻找运行在指定端口上的流量管理器
  BIND_SYNC(is_traffic_manager_running) << [this] (uint16_t port) ->R<bool>
  {
    return (TrafficManagerInfo.find(port) != TrafficManagerInfo.end());
  };

  /// 获取一个包含流量管理器的 <IP, port> 的键值对，该流量管理器运行在指定端口上。
  /// 如果没有流量管理器在运行，则返回的键值对为 ("", 0)。
  BIND_SYNC(get_traffic_manager_running) << [this] (uint16_t port) ->R<std::pair<std::string, uint16_t>>
  {
    auto it = TrafficManagerInfo.find(port);
    if(it != TrafficManagerInfo.end()) {
      return std::pair<std::string, uint16_t>(it->second, it->first);
    }
    return std::pair<std::string, uint16_t>("",0);
  };

  /// 添加在<IP，端口>上运行的新Traffic Manager
  BIND_SYNC(add_traffic_manager_running) << [this] (std::pair<std::string, uint16_t> trafficManagerInfo) ->R<bool>
  {
    uint16_t port = trafficManagerInfo.second;
    auto it = TrafficManagerInfo.find(port);
    if(it == TrafficManagerInfo.end()) {
      TrafficManagerInfo.insert(
        std::pair<uint16_t, std::string>(port, trafficManagerInfo.first));
      return true;
    }
    return false;

  };
// 绑定一个同步RPC函数，用于销毁指定端口的交通管理器
  BIND_SYNC(destroy_traffic_manager) << [this] (uint16_t port) ->R<bool>
  {
    // 在TrafficManagerInfo容器中查找指定端口的交通管理器信息
    auto it = TrafficManagerInfo.find(port);
    if(it != TrafficManagerInfo.end()) {
      TrafficManagerInfo.erase(it);
      return true;
    }
    return false;
  };
// 绑定一个异步RPC函数，用于获取CARLA的版本号
  BIND_ASYNC(version) << [] () -> R<std::string>
  {
    return carla::version();
  };

  // ~~ 时钟周期 ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  BIND_SYNC(tick_cue) << [this]() -> R<uint64_t>
  {
    TRACE_CPUPROFILER_EVENT_SCOPE(TickCueReceived);
    auto Current = FCarlaEngine::GetFrameCounter();
    (void)TickCuesReceived.fetch_add(1, std::memory_order_release);
    return Current + 1;
  };

  // ~~ 加载新章节 ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// 使用BIND_ASYNC宏来异步绑定一个函数，这里的函数是获取所有可用的地图名称
  BIND_ASYNC(get_available_maps) << [this]() -> R<std::vector<std::string>>
  {// 通过UCarlaStatics类的静态方法GetAllMapNames获取所有地图名称的列表
    const auto MapNames = UCarlaStatics::GetAllMapNames();
    // 创建一个std::vector来存储过滤后的地图名称
    std::vector<std::string> result;
    // 为result预留足够的空间，以提高性能（避免多次内存分配）
    // Num()是FString或类似容器的成员函数，返回容器中元素的数量
    result.reserve(MapNames.Num());
    // 遍历所有地图名称
    for (const auto &MapName : MapNames)
    {// 如果地图名称包含"/Sublevels/"，则跳过该地图（可能是子地图或层级地图）
      if (MapName.Contains("/Sublevels/"))
        continue;
      if (MapName.Contains("/BaseMap/"))
        continue;
      if (MapName.Contains("/BaseLargeMap/"))
        continue;
      if (MapName.Contains("_Tile_"))
        continue;
// 如果地图名称通过了所有过滤条件，则将其添加到结果列表中
// cr::FromFString是一个将FString转换为std::string的函数
      result.emplace_back(cr::FromFString(MapName));
    }
    // 返回过滤后的地图名称列表
    return result;
  };

  BIND_SYNC(load_new_episode) << [this](const std::string &map_name, const bool reset_settings, cr::MapLayer MapLayers) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();//检查当前是否存在有效的 CARLA 场景。如果不存在，它会抛出异常或返回错误

    UCarlaGameInstance* GameInstance = UCarlaStatics::GetGameInstance(Episode->GetWorld());
    if (!GameInstance)
    {
      RESPOND_ERROR("unable to find CARLA game instance");
    }
    GameInstance->SetMapLayer(static_cast<int32>(MapLayers));

    if(!Episode->LoadNewEpisode(cr::ToFString(map_name), reset_settings))
    {
      FString Str(TEXT("Map '"));
      Str += cr::ToFString(map_name);
      Str += TEXT("' not found");
      RESPOND_ERROR_FSTRING(Str);
    }

    return R<void>::Success();
  };
// 使用宏或模板函数绑定同步函数load_map_layer到下面的lambda表达式
  BIND_SYNC(load_map_layer) << [this](cr::MapLayer MapLayers) -> R<void>
  {
// 检查当前是否有一个有效的CARLA模拟场景（episode）正在运行
    REQUIRE_CARLA_EPISODE();
// 从当前运行的模拟世界中获取CARLA游戏模式（GameMode）的实例
    // ACarlaGameModeBase是CARLA中定义的游戏模式基类
    ACarlaGameModeBase* GameMode = UCarlaStatics::GetGameMode(Episode->GetWorld());
    if (!GameMode)
    {
      RESPOND_ERROR("unable to find CARLA game mode");
    }
     // 调用游戏模式的LoadMapLayer函数，加载指定的地图层
   // 将枚举类型转换为整型，因为函数可能需要整型参数
    GameMode->LoadMapLayer(static_cast<int32>(MapLayers));
  // 函数执行成功，返回成功状态的R<void>对象
    return R<void>::Success();
  };

  BIND_SYNC(unload_map_layer) << [this](cr::MapLayer MapLayers) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();

    ACarlaGameModeBase* GameMode = UCarlaStatics::GetGameMode(Episode->GetWorld());
    if (!GameMode)
    {
      RESPOND_ERROR("unable to find CARLA game mode");
    }
    GameMode->UnLoadMapLayer(static_cast<int32>(MapLayers));

    return R<void>::Success();
  };

  BIND_SYNC(copy_opendrive_to_file) << [this](const std::string &opendrive, cr::OpendriveGenerationParameters Params) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    //使用提供的OpenDRIVE数据和参数加载一个新的模拟场景
    // cr::ToLongFString(opendrive)将std::string转换为CARLA内部使用的FString类型
    // Params是OpenDRIVE生成参数，用于自定义加载过程
    if (!Episode->LoadNewOpendriveEpisode(cr::ToLongFString(opendrive), Params))
    {
      RESPOND_ERROR("opendrive could not be correctly parsed");
    }
    return R<void>::Success();
  };

  BIND_SYNC(apply_color_texture_to_objects) << [this](
      const std::vector<std::string> &actors_name,// 场景中要应用纹理的对象名称列表
      const cr::MaterialParameter& parameter,// 材料参数，用于定义纹理的应用方式
      const cr::TextureColor& Texture) -> R<void>// 纹理颜色参数
  {
 // 这是一个lambda函数或函数对象的定义，被设计为异步执行

// 返回一个R<void>类型的对象，这通常表示一个异步操作的结果
    REQUIRE_CARLA_EPISODE();// 检查当前是否有一个有效的CARLA模拟器会话正在运行
    ACarlaGameModeBase* GameMode = UCarlaStatics::GetGameMode(Episode->GetWorld());
    if (!GameMode)
    {
      RESPOND_ERROR("unable to find CARLA game mode");
    }
    TArray<AActor*> ActorsToPaint;// 创建一个数组来存储找到的actor指针
    for(const std::string& actor_name : actors_name)
    {
      AActor* ActorToPaint = GameMode->FindActorByName(cr::ToFString(actor_name));
      if (ActorToPaint)
      {
        ActorsToPaint.Add(ActorToPaint);
      }
    }

    if(!ActorsToPaint.Num())
    {
      RESPOND_ERROR("unable to find Actor to apply the texture");
    }

    UTexture2D* UETexture = GameMode->CreateUETexture(Texture);

    for(AActor* ActorToPaint : ActorsToPaint)
    {
      GameMode->ApplyTextureToActor(
          ActorToPaint,
          UETexture,
          parameter);
    }
    return R<void>::Success();
  };

  BIND_SYNC(apply_float_color_texture_to_objects) << [this](
      const std::vector<std::string> &actors_name,
      const cr::MaterialParameter& parameter,
      const cr::TextureFloatColor& Texture) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    ACarlaGameModeBase* GameMode = UCarlaStatics::GetGameMode(Episode->GetWorld());
    if (!GameMode)
    {
      RESPOND_ERROR("unable to find CARLA game mode");
    }
    TArray<AActor*> ActorsToPaint;
    for(const std::string& actor_name : actors_name)
    {
      AActor* ActorToPaint = GameMode->FindActorByName(cr::ToFString(actor_name));
      if (ActorToPaint)
      {
        ActorsToPaint.Add(ActorToPaint);
      }
    }

    if(!ActorsToPaint.Num())
    {
      RESPOND_ERROR("unable to find Actor to apply the texture");
    }

    UTexture2D* UETexture = GameMode->CreateUETexture(Texture);

    for(AActor* ActorToPaint : ActorsToPaint)
    {
      GameMode->ApplyTextureToActor(
          ActorToPaint,
          UETexture,
          parameter);
    }
    return R<void>::Success();
  };

  BIND_SYNC(get_names_of_all_objects) << [this]() -> R<std::vector<std::string>>
  {
    REQUIRE_CARLA_EPISODE();
    ACarlaGameModeBase* GameMode = UCarlaStatics::GetGameMode(Episode->GetWorld());
    if (!GameMode)
    {
      RESPOND_ERROR("unable to find CARLA game mode");
    }
    TArray<FString> NamesFString = GameMode->GetNamesOfAllActors();
    std::vector<std::string> NamesStd;
    for (const FString &Name : NamesFString)
    {
      NamesStd.emplace_back(cr::FromFString(Name));
    }
    return NamesStd;
  };

  // ~~ 章节设置与信息 ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  BIND_SYNC(get_episode_info) << [this]() -> R<cr::EpisodeInfo>
  {
    REQUIRE_CARLA_EPISODE();
    return cr::EpisodeInfo{Episode->GetId(), BroadcastStream.token()};
  };

  BIND_SYNC(get_map_info) << [this]() -> R<cr::MapInfo>
  {
    REQUIRE_CARLA_EPISODE();
    ACarlaGameModeBase* GameMode = UCarlaStatics::GetGameMode(Episode->GetWorld());
    const auto &SpawnPoints = Episode->GetRecommendedSpawnPoints();
    FString FullMapPath = GameMode->GetFullMapPath();
    FString MapDir = FullMapPath.RightChop(FullMapPath.Find("Content/", ESearchCase::CaseSensitive) + 8);
    MapDir += "/" + Episode->GetMapName();
    return cr::MapInfo{
      cr::FromFString(MapDir),
      MakeVectorFromTArray<cg::Transform>(SpawnPoints)};
  };

  BIND_SYNC(get_map_data) << [this]() -> R<std::string>
  {
    REQUIRE_CARLA_EPISODE();
    return cr::FromLongFString(UOpenDrive::GetXODR(Episode->GetWorld()));
  };

  BIND_SYNC(get_navigation_mesh) << [this]() -> R<std::vector<uint8_t>>
  {
    REQUIRE_CARLA_EPISODE();
    auto FileContents = FNavigationMesh::Load(Episode->GetMapName());
    // 进行内存复制（从TArray到std::vector）
    std::vector<uint8_t> Result(FileContents.Num());
    memcpy(&Result[0], FileContents.GetData(), FileContents.Num());
    return Result;
  };

  BIND_SYNC(get_required_files) << [this](std::string folder = "") -> R<std::vector<std::string>>
  {
    REQUIRE_CARLA_EPISODE();

    // 检查路径是否以斜杠结尾，如果没有则添加它
    if (folder[folder.size() - 1] != '/' && folder[folder.size() - 1] != '\\') {
      folder += "/";
    }

    // 获取地图文件夹的绝对路径，并检查它是否位于其自身的文件夹内
    ACarlaGameModeBase* GameMode = UCarlaStatics::GetGameMode(Episode->GetWorld());
    const auto mapDir = GameMode->GetFullMapPath();
    const auto folderDir = mapDir + "/" + folder.c_str();
    const auto fileName = mapDir.EndsWith(Episode->GetMapName()) ? "*" : Episode->GetMapName();

    // 从地图中找到所有的xodr和bin文件
    TArray<FString> Files;
    IFileManager::Get().FindFilesRecursive(Files, *folderDir, *(fileName + ".xodr"), true, false, false);
    IFileManager::Get().FindFilesRecursive(Files, *folderDir, *(fileName + ".bin"), true, false, false);

    // 移除路径的起始部分直到内容文件夹，并将每个文件放入结果中
    std::vector<std::string> result;
    for (auto File : Files) {
      File.RemoveFromStart(FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()));
      result.emplace_back(TCHAR_TO_UTF8(*File));
    }

    return result;
  };
  BIND_SYNC(request_file) << [this](std::string name) -> R<std::vector<uint8_t>>
  {
    REQUIRE_CARLA_EPISODE();

    // 获取文件的绝对路径
    FString path(FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()));
    path.Append(name.c_str());

    // 将文件的二进制数据复制到结果中并返回它
    TArray<uint8_t> Content;
    FFileHelper::LoadFileToArray(Content, *path, 0);
    std::vector<uint8_t> Result(Content.Num());
    memcpy(&Result[0], Content.GetData(), Content.Num());

    return Result;
  };

  BIND_SYNC(get_episode_settings) << [this]() -> R<cr::EpisodeSettings>
  {
    REQUIRE_CARLA_EPISODE();
    return cr::EpisodeSettings{Episode->GetSettings()};
  };

  BIND_SYNC(set_episode_settings) << [this](
      const cr::EpisodeSettings &settings) -> R<uint64_t>
  {
    REQUIRE_CARLA_EPISODE();
    Episode->ApplySettings(settings);
    StreamingServer.SetSynchronousMode(settings.synchronous_mode);

    ACarlaGameModeBase* GameMode = UCarlaStatics::GetGameMode(Episode->GetWorld());
    if (!GameMode)
    {
      RESPOND_ERROR("unable to find CARLA game mode");
    }
    ALargeMapManager* LargeMap = GameMode->GetLMManager();
    if (LargeMap)
    {
      LargeMap->ConsiderSpectatorAsEgo(settings.spectator_as_ego);
    }

    return FCarlaEngine::GetFrameCounter();
  };

  BIND_SYNC(get_actor_definitions) << [this]() -> R<std::vector<cr::ActorDefinition>>
  {
    REQUIRE_CARLA_EPISODE();
    return MakeVectorFromTArray<cr::ActorDefinition>(Episode->GetActorDefinitions());
  };

  BIND_SYNC(get_spectator) << [this]() -> R<cr::Actor>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(Episode->GetSpectatorPawn());
    if (!CarlaActor)
    {
      RESPOND_ERROR("internal error: unable to find spectator");
    }
    return Episode->SerializeActor(CarlaActor);
  };

  BIND_SYNC(get_all_level_BBs) << [this](uint8 QueriedTag) -> R<std::vector<cg::BoundingBox>>
  {
    REQUIRE_CARLA_EPISODE();
    TArray<FBoundingBox> Result;
    ACarlaGameModeBase* GameMode = UCarlaStatics::GetGameMode(Episode->GetWorld());
    if (!GameMode)
    {
      RESPOND_ERROR("unable to find CARLA game mode");
    }
    Result = GameMode->GetAllBBsOfLevel(QueriedTag);
    ALargeMapManager* LargeMap = GameMode->GetLMManager();
    if (LargeMap)
    {
      for(auto& Box : Result)
      {
        Box.Origin = LargeMap->LocalToGlobalLocation(Box.Origin);
      }
    }
    return MakeVectorFromTArray<cg::BoundingBox>(Result);
  };

  BIND_SYNC(get_environment_objects) << [this](uint8 QueriedTag) -> R<std::vector<cr::EnvironmentObject>>
  {
    REQUIRE_CARLA_EPISODE();
    ACarlaGameModeBase* GameMode = UCarlaStatics::GetGameMode(Episode->GetWorld());
    if (!GameMode)
    {
      RESPOND_ERROR("unable to find CARLA game mode");
    }
    TArray<FEnvironmentObject> Result = GameMode->GetEnvironmentObjects(QueriedTag);
    ALargeMapManager* LargeMap = GameMode->GetLMManager();
    if (LargeMap)
    {
      for(auto& Object : Result)
      {
        Object.Transform = LargeMap->LocalToGlobalTransform(Object.Transform);
      }
    }
    return MakeVectorFromTArray<cr::EnvironmentObject>(Result);
  };

  BIND_SYNC(enable_environment_objects) << [this](std::vector<uint64_t> EnvObjectIds, bool Enable) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    ACarlaGameModeBase* GameMode = UCarlaStatics::GetGameMode(Episode->GetWorld());
    if (!GameMode)
    {
      RESPOND_ERROR("unable to find CARLA game mode");
    }

    TSet<uint64> EnvObjectIdsSet;
    for(uint64 Id : EnvObjectIds)
    {
      EnvObjectIdsSet.Emplace(Id);
    }

    GameMode->EnableEnvironmentObjects(EnvObjectIdsSet, Enable);
    return R<void>::Success();
  };

  // ~~ Weather ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  BIND_SYNC(get_weather_parameters) << [this]() -> R<cr::WeatherParameters>
  {
    REQUIRE_CARLA_EPISODE();
    auto *Weather = Episode->GetWeather();
    if (Weather == nullptr)
    {
      RESPOND_ERROR("internal error: unable to find weather");
    }
    return Weather->GetCurrentWeather();
  };

  BIND_SYNC(set_weather_parameters) << [this](
      const cr::WeatherParameters &weather) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    auto *Weather = Episode->GetWeather();
    if (Weather == nullptr)
    {
      RESPOND_ERROR("internal error: unable to find weather");
    }
    Weather->ApplyWeather(weather);
    return R<void>::Success();
  };
  
  // -- IMUI Gravity ---------------------------------------------------------
  
  BIND_SYNC(get_imui_gravity) << [this]() -> R<float>
  {
    REQUIRE_CARLA_EPISODE();
    ACarlaGameModeBase* GameMode = UCarlaStatics::GetGameMode(Episode->GetWorld());
    if (GameMode == nullptr)
    {
      RESPOND_ERROR("get_imui_gravity error: unable to get carla gamemode");
    }
    return GameMode->IMUISensorGravity;
  };

  BIND_SYNC(set_imui_gravity) << [this](float newimuigravity) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    ACarlaGameModeBase* GameMode = UCarlaStatics::GetGameMode(Episode->GetWorld());
    if (GameMode == nullptr)
    {
      RESPOND_ERROR("get_imui_gravity error: unable to get carla gamemode");
    }
    GameMode->IMUISensorGravity = newimuigravity;
    return R<void>::Success();
  };

  // ~~ Actor operations ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  BIND_SYNC(get_actors_by_id) << [this](
      const std::vector<FCarlaActor::IdType> &ids) -> R<std::vector<cr::Actor>>
  {
    REQUIRE_CARLA_EPISODE();
   // 创建一个用于存储结果的向量，并预留足够的空间以优化性能
    std::vector<cr::Actor> Result;
    Result.reserve(ids.size());
    // 遍历提供的ID列表
    for (auto &&Id : ids)
    {// 使用ID在场景中查找对应的CARLA对象视图
      FCarlaActor* View = Episode->FindCarlaActor(Id);
      if (View)
      {
        Result.emplace_back(Episode->SerializeActor(View));
      }
    }
    return Result;
  };
// 绑定同步函数spawn_actor到下面的lambda表达式
  BIND_SYNC(spawn_actor) << [this](
      cr::ActorDescription Description,// 对象描述，包含类型、属性等信息
      const cr::Transform &Transform) -> R<cr::Actor>// 对象的位置和朝向
  {
    // 检查当前是否有一个有效的CARLA模拟场景正在运行
    REQUIRE_CARLA_EPISODE();

    auto Result = Episode->SpawnActorWithInfo(Transform, std::move(Description));

    if (Result.Key != EActorSpawnResultStatus::Success)
    {
      UE_LOG(LogCarla, Error, TEXT("Actor not Spawned"));
      RESPOND_ERROR_FSTRING(FActorSpawnResult::StatusToString(Result.Key));
    }

    ALargeMapManager* LargeMap = UCarlaStatics::GetLargeMapManager(Episode->GetWorld());
    if(LargeMap)
    {
      LargeMap->OnActorSpawned(*Result.Value);
    }

    return Episode->SerializeActor(Result.Value);
  };

  BIND_SYNC(spawn_actor_with_parent) << [this](
      cr::ActorDescription Description,
      const cr::Transform &Transform,
      cr::ActorId ParentId,
      cr::AttachmentType InAttachmentType,
      const std::string& socket_name) -> R<cr::Actor>
  {
    REQUIRE_CARLA_EPISODE();
   // 尝试在模拟场景中生成一个新的actor
   // Transform包含了新actor的位置和朝向信息
   // Description包含了新actor的类型、属性等描述信息
    auto Result = Episode->SpawnActorWithInfo(Transform, std::move(Description));
   // 检查生成actor的结果 
    if (Result.Key != EActorSpawnResultStatus::Success)
    {
      RESPOND_ERROR_FSTRING(FActorSpawnResult::StatusToString(Result.Key));
    }

    FCarlaActor* CarlaActor = Episode->FindCarlaActor(Result.Value->GetActorId());
    if (!CarlaActor)
    {
      RESPOND_ERROR("internal error: actor could not be spawned");
    }

    FCarlaActor* ParentCarlaActor = Episode->FindCarlaActor(ParentId);

    if (!ParentCarlaActor)
    {
      RESPOND_ERROR("unable to attach actor: parent actor not found");
    }

    CarlaActor->SetParent(ParentId);
    CarlaActor->SetAttachmentType(InAttachmentType);
    ParentCarlaActor->AddChildren(CarlaActor->GetActorId());

    #if defined(WITH_ROS2)
    auto ROS2 = carla::ros2::ROS2::GetInstance();
    if (ROS2->IsEnabled())
    {
      FCarlaActor* CurrentActor = ParentCarlaActor;
      while(CurrentActor)
      {
        for (const auto &Attr : CurrentActor->GetActorInfo()->Description.Variations)
        {
          if (Attr.Key == "ros_name")
          {
            const std::string value = std::string(TCHAR_TO_UTF8(*Attr.Value.Value));
            ROS2->AddActorParentRosName(static_cast<void*>(CarlaActor->GetActor()), static_cast<void*>(CurrentActor->GetActor()));
          }
        }
        CurrentActor = Episode->FindCarlaActor(CurrentActor->GetParent());
      }
    }
    #endif

    // 只有在actor确实已经被生成（spawned）并且不处于休眠状态时，才能进行附加（attach）操作。
    if(!ParentCarlaActor->IsDormant())
    {
      Episode->AttachActors(
          CarlaActor->GetActor(),
          ParentCarlaActor->GetActor(),
          static_cast<EAttachmentType>(InAttachmentType),
          FString(socket_name.c_str()));
    }
    else
    {
      Episode->PutActorToSleep(CarlaActor->GetActorId());
    }

    return Episode->SerializeActor(CarlaActor);
  };

  BIND_SYNC(destroy_actor) << [this](cr::ActorId ActorId) -> R<bool>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if ( !CarlaActor )
    {
      RESPOND_ERROR("unable to destroy actor: not found");
    }
    UE_LOG(LogCarla, Log, TEXT("CarlaServer destroy_actor %d"), ActorId);
    // 我们需要强制改变actor的状态，因为处于休眠状态的actors会忽略FCarlaActor的销毁指令。
    CarlaActor->SetActorState(cr::ActorState::PendingKill);
    if (!Episode->DestroyActor(ActorId))
    {
      RESPOND_ERROR("internal error: unable to destroy actor");
    }
    return true;
  };

  BIND_SYNC(console_command) << [this](std::string cmd) -> R<bool>
  {
    REQUIRE_CARLA_EPISODE();
    APlayerController* PController= UGameplayStatics::GetPlayerController(Episode->GetWorld(), 0);
    if( PController )
    {
        auto result = PController->ConsoleCommand(UTF8_TO_TCHAR(cmd.c_str()), true);
        return !(
          result.Contains(FString(TEXT("Command not recognized"))) ||
          result.Contains(FString(TEXT("Error")))
        );
    }
    return GEngine->Exec(Episode->GetWorld(), UTF8_TO_TCHAR(cmd.c_str()));
  };

  BIND_SYNC(get_sensor_token) << [this](carla::streaming::detail::stream_id_type sensor_id) ->
                                 R<carla::streaming::Token>
  {
    REQUIRE_CARLA_EPISODE();
    bool ForceInPrimary = false;

    // check for the world observer (always in primary server)
    if (sensor_id == 1)
    {
      ForceInPrimary = true;
    }

    // collision sensor always in primary server in multi-gpu
    FString Desc = Episode->GetActorDescriptionFromStream(sensor_id);
    if (Desc == "" || Desc == "sensor.other.collision")
    {
      ForceInPrimary = true;
    }

    if (SecondaryServer->HasClientsConnected() && !ForceInPrimary)
    {
      // multi-gpu
      UE_LOG(LogCarla, Log, TEXT("Sensor %d '%s' created in secondary server"), sensor_id, *Desc);
      return SecondaryServer->GetCommander().GetToken(sensor_id);
    }
    else
    {
      // single-gpu
      UE_LOG(LogCarla, Log, TEXT("Sensor %d '%s' created in primary server"), sensor_id, *Desc);
      return StreamingServer.GetToken(sensor_id);
    }
  };

  BIND_SYNC(enable_sensor_for_ros) << [this](carla::streaming::detail::stream_id_type sensor_id) ->
                                 R<void>
  {
    REQUIRE_CARLA_EPISODE();
    bool ForceInPrimary = false;

    // check for the world observer (always in primary server)
    if (sensor_id == 1)
    {
      ForceInPrimary = true;
    }

    // collision sensor always in primary server in multi-gpu
    FString Desc = Episode->GetActorDescriptionFromStream(sensor_id);
    if (Desc == "" || Desc == "sensor.other.collision")
    {
      ForceInPrimary = true;
    }

    if (SecondaryServer->HasClientsConnected() && !ForceInPrimary)
    {
      // multi-gpu
      SecondaryServer->GetCommander().EnableForROS(sensor_id);
    }
    else
    {
      // single-gpu
      StreamingServer.EnableForROS(sensor_id);
    }
    return R<void>::Success();
  };

  BIND_SYNC(disable_sensor_for_ros) << [this](carla::streaming::detail::stream_id_type sensor_id) ->
                                 R<void>
  {
    REQUIRE_CARLA_EPISODE();
    bool ForceInPrimary = false;

    // check for the world observer (always in primary server)
    if (sensor_id == 1)
    {
      ForceInPrimary = true;
    }

    // collision sensor always in primary server in multi-gpu
    FString Desc = Episode->GetActorDescriptionFromStream(sensor_id);
    if (Desc == "" || Desc == "sensor.other.collision")
    {
      ForceInPrimary = true;
    }

    if (SecondaryServer->HasClientsConnected() && !ForceInPrimary)
    {
      // multi-gpu
      SecondaryServer->GetCommander().DisableForROS(sensor_id);
    }
    else
    {
      // single-gpu
      StreamingServer.DisableForROS(sensor_id);
    }
    return R<void>::Success();
  };

BIND_SYNC(is_sensor_enabled_for_ros) << [this](carla::streaming::detail::stream_id_type sensor_id) ->
                                 R<bool>
  {
    REQUIRE_CARLA_EPISODE();
    bool ForceInPrimary = false;

    // check for the world observer (always in primary server)
    if (sensor_id == 1)
    {
      ForceInPrimary = true;
    }

    // collision sensor always in primary server in multi-gpu
    FString Desc = Episode->GetActorDescriptionFromStream(sensor_id);
    if (Desc == "" || Desc == "sensor.other.collision")
    {
      ForceInPrimary = true;
    }

    if (SecondaryServer->HasClientsConnected() && !ForceInPrimary)
    {
      // multi-gpu
      return SecondaryServer->GetCommander().IsEnabledForROS(sensor_id);
    }
    else
    {
      // single-gpu
      return StreamingServer.IsEnabledForROS(sensor_id);
    }
  };


  BIND_SYNC(send) << [this](
      cr::ActorId ActorId,
      std::string message) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "send",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }

    if (CarlaActor->IsDormant())
    {
      return RespondError(
          "send",
          ECarlaServerResponse::FunctionNotAvailiableWhenDormant,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ACustomV2XSensor* Sensor = Cast<ACustomV2XSensor>(CarlaActor->GetActor());
    if (!Sensor)
    {
      return RespondError(
        "send",
        ECarlaServerResponse::ActorTypeMismatch,
        " Actor Id: " + FString::FromInt(ActorId));
    }

    Sensor->Send(cr::ToFString(message));
    return R<void>::Success();
  };

  // ~~ Actor physics ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  BIND_SYNC(set_actor_location) << [this](
      cr::ActorId ActorId,
      cr::Location Location) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "set_actor_location",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }

    CarlaActor->SetActorGlobalLocation(
        Location, ETeleportType::TeleportPhysics);
    return R<void>::Success();
  };

  BIND_SYNC(set_actor_transform) << [this](
      cr::ActorId ActorId,
      cr::Transform Transform) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "set_actor_transform",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }

    CarlaActor->SetActorGlobalTransform(
        Transform, ETeleportType::TeleportPhysics);
    return R<void>::Success();
  };

  BIND_SYNC(set_walker_state) << [this] (
      cr::ActorId ActorId,
      cr::Transform Transform,
      float Speed) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "set_walker_state",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }

    // apply walker transform
    ECarlaServerResponse Response =
        CarlaActor->SetWalkerState(
            Transform,
            cr::WalkerControl(
              Transform.GetForwardVector(), Speed, false));
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "set_walker_state",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };
// 使用BIND_SYNC宏绑定一个名为set_actor_target_velocity的同步操作
// 该操作接受一个actor的ID（cr::ActorId）和一个三维向量（cr::Vector3D）作为参数
// 并返回一个R<void>类型的响应对象，表示操作的结果
  BIND_SYNC(set_actor_target_velocity) << [this](
      cr::ActorId ActorId,// actor的唯一标识符
      cr::Vector3D vector) -> R<void>// actor应达到的目标速度向量
  {                                  
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "set_actor_target_velocity",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->SetActorTargetVelocity(vector.ToCentimeters().ToFVector());
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "set_actor_target_velocity",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(set_actor_target_angular_velocity) << [this](
      cr::ActorId ActorId,
      cr::Vector3D vector) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "set_actor_target_angular_velocity",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->SetActorTargetAngularVelocity(vector.ToFVector());
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "set_actor_target_angular_velocity",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(enable_actor_constant_velocity) << [this](
      cr::ActorId ActorId,
      cr::Vector3D vector) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "enable_actor_constant_velocity",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }

    ECarlaServerResponse Response =
        CarlaActor->EnableActorConstantVelocity(vector.ToCentimeters().ToFVector());
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "enable_actor_constant_velocity",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }

    return R<void>::Success();
  };

  BIND_SYNC(disable_actor_constant_velocity) << [this](
      cr::ActorId ActorId) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "disable_actor_constant_velocity",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }

    ECarlaServerResponse Response =
        CarlaActor->DisableActorConstantVelocity();
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "disable_actor_constant_velocity",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }

    return R<void>::Success();
  };

  BIND_SYNC(add_actor_impulse) << [this](
      cr::ActorId ActorId,
      cr::Vector3D vector) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "add_actor_impulse",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }

    ECarlaServerResponse Response =
        CarlaActor->AddActorImpulse(vector.ToCentimeters().ToFVector());
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "add_actor_impulse",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(add_actor_impulse_at_location) << [this](
      cr::ActorId ActorId,
      cr::Vector3D impulse,
      cr::Vector3D location) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "add_actor_impulse_at_location",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    FVector UELocation = location.ToCentimeters().ToFVector();
    ACarlaGameModeBase* GameMode = UCarlaStatics::GetGameMode(Episode->GetWorld());
    ALargeMapManager* LargeMap = GameMode->GetLMManager();
    if (LargeMap)
    {
      UELocation = LargeMap->GlobalToLocalLocation(UELocation);
    }
    ECarlaServerResponse Response =
        CarlaActor->AddActorImpulseAtLocation(impulse.ToCentimeters().ToFVector(), UELocation);
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "add_actor_impulse_at_location",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }

    return R<void>::Success();
  };

  BIND_SYNC(add_actor_force) << [this](
      cr::ActorId ActorId,
      cr::Vector3D vector) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "add_actor_force",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->AddActorForce(vector.ToCentimeters().ToFVector());
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "add_actor_force",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(add_actor_force_at_location) << [this](
      cr::ActorId ActorId,
      cr::Vector3D force,
      cr::Vector3D location) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "add_actor_force_at_location",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    FVector UELocation = location.ToCentimeters().ToFVector();
    ACarlaGameModeBase* GameMode = UCarlaStatics::GetGameMode(Episode->GetWorld());
    ALargeMapManager* LargeMap = GameMode->GetLMManager();
    if (LargeMap)
    {
      UELocation = LargeMap->GlobalToLocalLocation(UELocation);
    }
    ECarlaServerResponse Response =
        CarlaActor->AddActorForceAtLocation(UELocation, force.ToCentimeters().ToFVector());
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "add_actor_force_at_location",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(add_actor_angular_impulse) << [this](
      cr::ActorId ActorId,
      cr::Vector3D vector) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "add_actor_angular_impulse",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->AddActorAngularImpulse(vector.ToFVector());
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "add_actor_angular_impulse",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(add_actor_torque) << [this](
      cr::ActorId ActorId,
      cr::Vector3D vector) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "add_actor_torque",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->AddActorTorque(vector.ToFVector());
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "add_actor_torque",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(get_actor_component_world_transform) << [this](
      cr::ActorId ActorId,
      const std::string componentName) -> R<cr::Transform>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "get_actor_component_world_transform",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    else
    {
      TArray<UActorComponent*> Components;
      CarlaActor->GetActor()->GetComponents(Components);

      USceneComponent* Component = nullptr;
      for(auto Cmp : Components)
      {
        if(USceneComponent* SCMP = Cast<USceneComponent>(Cmp))
        {
          if(SCMP->GetName() == componentName.c_str())
          {
            Component = SCMP;
            break;
          }
        }
      }

      if(!Component)
      {
        return RespondError(
            "get_actor_component_world_transform",
            ECarlaServerResponse::ComponentNotFound,
            " Component Name: " + FString(componentName.c_str()));
      }

      FTransform ComponentWorldTransform = Component->GetComponentTransform();
      return cr::Transform(ComponentWorldTransform);
    }
  };

  BIND_SYNC(get_actor_component_relative_transform) << [this](
      cr::ActorId ActorId,
      const std::string componentName) -> R<cr::Transform>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "get_actor_component_relative_transform",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    else
    {
      TArray<UActorComponent*> Components;
      CarlaActor->GetActor()->GetComponents(Components);

      USceneComponent* Component = nullptr;
      for(auto Cmp : Components)
      {
        if(USceneComponent* SCMP = Cast<USceneComponent>(Cmp))
        {
          if(SCMP->GetName() == componentName.c_str())
          {
            Component = SCMP;
            break;
          }
        }
      }

      if(!Component)
      {
        return RespondError(
            "get_actor_component_world_transform",
            ECarlaServerResponse::ComponentNotFound,
            " Component Name: " + FString(componentName.c_str()));
      }

      FTransform ComponentRelativeTransform = Component->GetRelativeTransform();
      return cr::Transform(ComponentRelativeTransform);
    }
  };

  BIND_SYNC(get_actor_bone_world_transforms) << [this](
      cr::ActorId ActorId) -> R<std::vector<cr::Transform>>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "get_actor_bone_world_transforms",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    else
    {
      TArray<FTransform> BoneWorldTransforms;
      TArray<USkinnedMeshComponent*> SkinnedMeshComponents;
      CarlaActor->GetActor()->GetComponents<USkinnedMeshComponent>(SkinnedMeshComponents);
      if(!SkinnedMeshComponents[0])
      {
        return RespondError(
            "get_actor_bone_world_transforms",
            ECarlaServerResponse::ComponentNotFound,
            " Component Name: SkinnedMeshComponent ");
      }
      else
      {
        for(USkinnedMeshComponent* SkinnedMeshComponent : SkinnedMeshComponents)
        {
          const int32 NumBones = SkinnedMeshComponent->GetNumBones();
          for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
          {
            FTransform WorldTransform = SkinnedMeshComponent->GetComponentTransform();
            FTransform BoneTransform = SkinnedMeshComponent->GetBoneTransform(BoneIndex, WorldTransform);
            BoneWorldTransforms.Add(BoneTransform);  
          }
        }
        return MakeVectorFromTArray<cr::Transform>(BoneWorldTransforms);
      }      
    }
  };

  BIND_SYNC(get_actor_bone_relative_transforms) << [this](
      cr::ActorId ActorId) -> R<std::vector<cr::Transform>>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "get_actor_bone_relative_transforms",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    else
    {
      TArray<FTransform> BoneRelativeTransforms;
      TArray<USkinnedMeshComponent*> SkinnedMeshComponents;
      CarlaActor->GetActor()->GetComponents<USkinnedMeshComponent>(SkinnedMeshComponents);
      if(!SkinnedMeshComponents[0])
      {
        return RespondError(
            "get_actor_bone_relative_transforms",
            ECarlaServerResponse::ComponentNotFound,
            " Component Name: SkinnedMeshComponent ");
      }
      else
      {
        for(USkinnedMeshComponent* SkinnedMeshComponent : SkinnedMeshComponents)
        {
          const int32 NumBones = SkinnedMeshComponent->GetNumBones();
          for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
          {
            FTransform BoneTransform = SkinnedMeshComponent->GetBoneTransform(BoneIndex, FTransform::Identity);
            BoneRelativeTransforms.Add(BoneTransform);  
          }
        }
        return MakeVectorFromTArray<cr::Transform>(BoneRelativeTransforms);
      }
    }
  };

  BIND_SYNC(get_actor_component_names) << [this](
      cr::ActorId ActorId) -> R<std::vector<std::string>>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "get_actor_component_names",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    else
    {
      TArray<UActorComponent*> Components;
      CarlaActor->GetActor()->GetComponents(Components);
      std::vector<std::string> ComponentNames;
      for(auto Cmp : Components)
      {
        FString ComponentName = Cmp->GetName();
        ComponentNames.push_back(TCHAR_TO_UTF8(*ComponentName));
      }  
      return ComponentNames; 
    }
  };

  BIND_SYNC(get_actor_bone_names) << [this](
      cr::ActorId ActorId) -> R<std::vector<std::string>>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "get_actor_bone_names",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    else
    {
      USkinnedMeshComponent* SkinnedMeshComponent = CarlaActor->GetActor()->FindComponentByClass<USkinnedMeshComponent>();
      if(!SkinnedMeshComponent)   
      {
        return RespondError(
            "get_actor_bone_names",
            ECarlaServerResponse::ComponentNotFound,
            " Component Name: SkinnedMeshComponent ");    
      }  
      else
      {
        TArray<FName> BoneNames;
        SkinnedMeshComponent->GetBoneNames(BoneNames);
        TArray<std::string> StringBoneNames;
        for (const FName& Name : BoneNames)
        {
          FString FBoneName = Name.ToString();
          std::string StringBoneName = TCHAR_TO_UTF8(*FBoneName);
          StringBoneNames.Add(StringBoneName);
        }
        return MakeVectorFromTArray<std::string>(StringBoneNames);
      }
    }
  };

  BIND_SYNC(get_actor_socket_world_transforms) << [this](
      cr::ActorId ActorId) -> R<std::vector<cr::Transform>>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "get_actor_socket_world_transforms",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    else
    {
      TArray<FTransform> SocketWorldTransforms;
      TArray<UActorComponent*> Components;
      CarlaActor->GetActor()->GetComponents(Components);     
      for(UActorComponent* ActorComponent : Components)
      {
        if(USceneComponent* SceneComponent = Cast<USceneComponent>(ActorComponent))
        {
          const TArray<FName>& SocketNames = SceneComponent->GetAllSocketNames();        
          for (const FName& SocketName : SocketNames)
          {
            FTransform SocketTransform = SceneComponent->GetSocketTransform(SocketName);
            SocketWorldTransforms.Add(SocketTransform);
          }
        }
      }
      return MakeVectorFromTArray<cr::Transform>(SocketWorldTransforms);   
    }
  };

  BIND_SYNC(get_actor_socket_relative_transforms) << [this](
      cr::ActorId ActorId) -> R<std::vector<cr::Transform>>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "get_actor_socket_relative_transforms",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    else
    {
      TArray<FTransform> SocketRelativeTransforms;
      TArray<UActorComponent*> Components;
      CarlaActor->GetActor()->GetComponents(Components);     
      for(UActorComponent* ActorComponent : Components)
      {
        if(USceneComponent* SceneComponent = Cast<USceneComponent>(ActorComponent))
        {
          const TArray<FName>& SocketNames = SceneComponent->GetAllSocketNames();        
          for (const FName& SocketName : SocketNames)
          {
            FTransform SocketTransform = SceneComponent->GetSocketTransform(SocketName, ERelativeTransformSpace::RTS_Actor);
            SocketRelativeTransforms.Add(SocketTransform);
          }
        }
      }
      return MakeVectorFromTArray<cr::Transform>(SocketRelativeTransforms);
    }
  };

  BIND_SYNC(get_actor_socket_names) << [this](
      cr::ActorId ActorId) -> R<std::vector<std::string>>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "get_actor_socket_names",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    else
    {
      TArray<FName> SocketNames;
      std::vector<std::string> StringSocketNames;
      TArray<UActorComponent*> Components;
      CarlaActor->GetActor()->GetComponents(Components);     
      for(UActorComponent* ActorComponent : Components)
      {
        if(USceneComponent* SceneComponent = Cast<USceneComponent>(ActorComponent))
        {
          SocketNames = SceneComponent->GetAllSocketNames();    
          for (const FName& Name : SocketNames)
          {
            FString FSocketName = Name.ToString();
            std::string StringSocketName = TCHAR_TO_UTF8(*FSocketName);
            StringSocketNames.push_back(StringSocketName);
          }              
        }
      }
      return StringSocketNames;      
    }
  };

  // 向客户端公开遥测数据
  BIND_SYNC(get_physics_control) << [this](
      cr::ActorId ActorId) -> R<cr::VehiclePhysicsControl>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
        if (!CarlaActor)
    {
      return RespondError(
          "get_physics_control",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    FVehiclePhysicsControl PhysicsControl;
    ECarlaServerResponse Response =
        CarlaActor->GetPhysicsControl(PhysicsControl);  // 获得车辆的物理控制
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "get_physics_control",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return cr::VehiclePhysicsControl(PhysicsControl);
  };

  BIND_SYNC(get_vehicle_light_state) << [this](
      cr::ActorId ActorId) -> R<cr::VehicleLightState>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "get_vehicle_light_state",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    FVehicleLightState LightState;
    ECarlaServerResponse Response =
        CarlaActor->GetVehicleLightState(LightState);
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "get_vehicle_light_state",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return cr::VehicleLightState(LightState);
  };

  BIND_SYNC(apply_physics_control) << [this](
      cr::ActorId ActorId,
      cr::VehiclePhysicsControl PhysicsControl) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "apply_physics_control",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->ApplyPhysicsControl(FVehiclePhysicsControl(PhysicsControl));
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "apply_physics_control",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(set_vehicle_light_state) << [this](
      cr::ActorId ActorId,
      cr::VehicleLightState LightState) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "set_vehicle_light_state",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->SetVehicleLightState(FVehicleLightState(LightState));
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "set_vehicle_light_state",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };


  BIND_SYNC(open_vehicle_door) << [this](
      cr::ActorId ActorId,
      cr::VehicleDoor DoorIdx) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "open_vehicle_door",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->OpenVehicleDoor(static_cast<EVehicleDoor>(DoorIdx));
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "open_vehicle_door",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(close_vehicle_door) << [this](
      cr::ActorId ActorId,
      cr::VehicleDoor DoorIdx) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "close_vehicle_door",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->CloseVehicleDoor(static_cast<EVehicleDoor>(DoorIdx));
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "close_vehicle_door",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(set_wheel_steer_direction) << [this](
    cr::ActorId ActorId,
    cr::VehicleWheelLocation WheelLocation,
    float AngleInDeg) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if(!CarlaActor){
      return RespondError(
          "set_wheel_steer_direction",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->SetWheelSteerDirection(
            static_cast<EVehicleWheelLocation>(WheelLocation), AngleInDeg);
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "set_wheel_steer_direction",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(get_wheel_steer_angle) << [this](
      const cr::ActorId ActorId,
      cr::VehicleWheelLocation WheelLocation) -> R<float>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if(!CarlaActor){
      return RespondError(
          "get_wheel_steer_angle",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    float Angle;
    ECarlaServerResponse Response =
        CarlaActor->GetWheelSteerAngle(
            static_cast<EVehicleWheelLocation>(WheelLocation), Angle);
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "get_wheel_steer_angle",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return Angle;
  };

  BIND_SYNC(set_actor_simulate_physics) << [this](
      cr::ActorId ActorId,
      bool bEnabled) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "set_actor_simulate_physics",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->SetActorSimulatePhysics(bEnabled);
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "set_actor_simulate_physics",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(set_actor_collisions) << [this](
      cr::ActorId ActorId,
      bool bEnabled) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "set_actor_collisions",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->SetActorCollisions(bEnabled);
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "set_actor_collisions",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(set_actor_dead) << [this](
      cr::ActorId ActorId) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "set_actor_dead",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->SetActorDead();
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "set_actor_dead",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(set_actor_enable_gravity) << [this](
      cr::ActorId ActorId,
      bool bEnabled) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "set_actor_enable_gravity",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->SetActorEnableGravity(bEnabled);
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "set_actor_enable_gravity",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  // ~~ Apply control ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  BIND_SYNC(apply_control_to_vehicle) << [this](
      cr::ActorId ActorId,
      cr::VehicleControl Control) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "apply_control_to_vehicle",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->ApplyControlToVehicle(Control, EVehicleInputPriority::Client);
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "apply_control_to_vehicle",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(apply_ackermann_control_to_vehicle) << [this](
      cr::ActorId ActorId,
      cr::VehicleAckermannControl Control) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "apply_ackermann_control_to_vehicle",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->ApplyAckermannControlToVehicle(Control, EVehicleInputPriority::Client);
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "apply_ackermann_control_to_vehicle",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(get_ackermann_controller_settings) << [this](
      cr::ActorId ActorId) -> R<cr::AckermannControllerSettings>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
        if (!CarlaActor)
    {
      return RespondError(
          "get_ackermann_controller_settings",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    FAckermannControllerSettings Settings;
    ECarlaServerResponse Response =
        CarlaActor->GetAckermannControllerSettings(Settings);
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "get_ackermann_controller_settings",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return cr::AckermannControllerSettings(Settings);
  };

  BIND_SYNC(apply_ackermann_controller_settings) << [this](
      cr::ActorId ActorId,
      cr::AckermannControllerSettings AckermannSettings) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "apply_ackermann_controller_settings",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->ApplyAckermannControllerSettings(FAckermannControllerSettings(AckermannSettings));
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "apply_ackermann_controller_settings",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(apply_control_to_walker) << [this](
      cr::ActorId ActorId,
      cr::WalkerControl Control) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "apply_control_to_walker",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->ApplyControlToWalker(Control);
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "apply_control_to_walker",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(get_bones_transform) << [this](
      cr::ActorId ActorId) -> R<cr::WalkerBoneControlOut>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "get_bones_transform",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    FWalkerBoneControlOut Bones;
    ECarlaServerResponse Response =
        CarlaActor->GetBonesTransform(Bones);
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "get_bones_transform",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }

    std::vector<carla::rpc::BoneTransformDataOut> BoneData;
    for (auto Bone : Bones.BoneTransforms)
    {
      carla::rpc::BoneTransformDataOut Data;
      Data.bone_name = std::string(TCHAR_TO_UTF8(*Bone.Get<0>()));
      FWalkerBoneControlOutData Transforms = Bone.Get<1>();
      Data.world = Transforms.World;
      Data.component = Transforms.Component;
      Data.relative = Transforms.Relative;
      BoneData.push_back(Data);
    }
    return carla::rpc::WalkerBoneControlOut(BoneData);
  };

  BIND_SYNC(set_bones_transform) << [this](
      cr::ActorId ActorId,
      carla::rpc::WalkerBoneControlIn Bones) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "set_bones_transform",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }

    FWalkerBoneControlIn Bones2 = FWalkerBoneControlIn(Bones);
    ECarlaServerResponse Response = CarlaActor->SetBonesTransform(Bones2);
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "set_bones_transform",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }

    return R<void>::Success();
  };

  BIND_SYNC(blend_pose) << [this](
      cr::ActorId ActorId,
      float Blend) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "blend_pose",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }

    ECarlaServerResponse Response = CarlaActor->BlendPose(Blend);
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "blend_pose",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }

    return R<void>::Success();
  };

  BIND_SYNC(get_pose_from_animation) << [this](
      cr::ActorId ActorId) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "get_pose_from_animation",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }

    ECarlaServerResponse Response = CarlaActor->GetPoseFromAnimation();
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "get_pose_from_animation",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }

    return R<void>::Success();
  };

  BIND_SYNC(set_actor_autopilot) << [this](
      cr::ActorId ActorId,
      bool bEnabled) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "set_actor_autopilot",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->SetActorAutopilot(bEnabled);
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "set_actor_autopilot",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(get_telemetry_data) << [this](
      cr::ActorId ActorId) -> R<cr::VehicleTelemetryData>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
        if (!CarlaActor)
    {
      return RespondError(
          "get_telemetry_data",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    FVehicleTelemetryData TelemetryData;
    ECarlaServerResponse Response =
        CarlaActor->GetVehicleTelemetryData(TelemetryData);
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "get_telemetry_data",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return cr::VehicleTelemetryData(TelemetryData);
  };

  BIND_SYNC(show_vehicle_debug_telemetry) << [this](
      cr::ActorId ActorId,
      bool bEnabled) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "show_vehicle_debug_telemetry",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->ShowVehicleDebugTelemetry(bEnabled);
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "show_vehicle_debug_telemetry",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(enable_carsim) << [this](
      cr::ActorId ActorId,
      std::string SimfilePath) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "enable_carsim",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->EnableCarSim(carla::rpc::ToFString(SimfilePath));
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "enable_carsim",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(use_carsim_road) << [this](
      cr::ActorId ActorId,
      bool bEnabled) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "use_carsim_road",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->UseCarSimRoad(bEnabled);
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "use_carsim_road",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(enable_chrono_physics) << [this](
      cr::ActorId ActorId,
      uint64_t MaxSubsteps,
      float MaxSubstepDeltaTime,
      std::string VehicleJSON,
      std::string PowertrainJSON,
      std::string TireJSON,
      std::string BaseJSONPath) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "enable_chrono_physics",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->EnableChronoPhysics(
            MaxSubsteps, MaxSubstepDeltaTime,
            cr::ToFString(VehicleJSON),
            cr::ToFString(PowertrainJSON),
            cr::ToFString(TireJSON),
            cr::ToFString(BaseJSONPath));
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "enable_chrono_physics",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(restore_physx_physics) << [this](
      cr::ActorId ActorId) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "restore_physx_physics",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->RestorePhysXPhysics();
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "restore_physx_physics",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  // ~~ Traffic lights ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  BIND_SYNC(set_traffic_light_state) << [this](
      cr::ActorId ActorId,
      cr::TrafficLightState trafficLightState) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "set_traffic_light_state",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->SetTrafficLightState(
        static_cast<ETrafficLightState>(trafficLightState));
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "set_traffic_light_state",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(set_traffic_light_green_time) << [this](
      cr::ActorId ActorId,
      float GreenTime) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "set_traffic_light_green_time",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->SetLightGreenTime(GreenTime);
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "set_traffic_light_green_time",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(set_traffic_light_yellow_time) << [this](
      cr::ActorId ActorId,
      float YellowTime) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "set_traffic_light_yellow_time",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->SetLightYellowTime(YellowTime);
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "set_traffic_light_yellow_time",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(set_traffic_light_red_time) << [this](
      cr::ActorId ActorId,
      float RedTime) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "set_traffic_light_red_time",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->SetLightRedTime(RedTime);
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "set_traffic_light_red_time",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(freeze_traffic_light) << [this](
      cr::ActorId ActorId,
      bool Freeze) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "freeze_traffic_light",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->FreezeTrafficLight(Freeze);
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "freeze_traffic_light",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(reset_traffic_light_group) << [this](
      cr::ActorId ActorId) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "reset_traffic_light_group",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ECarlaServerResponse Response =
        CarlaActor->ResetTrafficLightGroup();
    if (Response != ECarlaServerResponse::Success)
    {
      return RespondError(
          "reset_traffic_light_group",
          Response,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    return R<void>::Success();
  };

  BIND_SYNC(reset_all_traffic_lights) << [this]() -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    for (TActorIterator<ATrafficLightGroup> It(Episode->GetWorld()); It; ++It)
    {
      It->ResetGroup();
    }
    return R<void>::Success();
  };

  BIND_SYNC(freeze_all_traffic_lights) << [this]
      (bool frozen) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    auto* GameMode = UCarlaStatics::GetGameMode(Episode->GetWorld());
    if (!GameMode)
    {
      RESPOND_ERROR("unable to find CARLA game mode");
    }
    auto* TraffiLightManager = GameMode->GetTrafficLightManager();
    TraffiLightManager->SetFrozen(frozen);
    return R<void>::Success();
  };

  BIND_SYNC(get_vehicle_light_states) << [this]() -> R<cr::VehicleLightStateList>
  {
    REQUIRE_CARLA_EPISODE();
    cr::VehicleLightStateList List;

    auto It = Episode->GetActorRegistry().begin();
    for (; It != Episode->GetActorRegistry().end(); ++It)
    {
      const FCarlaActor& View = *(It.Value().Get());
      if (View.GetActorType() == FCarlaActor::ActorType::Vehicle)
      {
        if(View.IsDormant())
        {
          // todo: implement
        }
        else
        {
          auto Actor = View.GetActor();
          if (!Actor->IsPendingKill())
          {
            const ACarlaWheeledVehicle *Vehicle = Cast<ACarlaWheeledVehicle>(Actor);
            List.emplace_back(
                View.GetActorId(),
                cr::VehicleLightState(Vehicle->GetVehicleLightState()).GetLightStateAsValue());
          }
        }
      }
    }
    return List;
  };

  BIND_SYNC(get_group_traffic_lights) << [this](
      const cr::ActorId ActorId) -> R<std::vector<cr::ActorId>>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      RESPOND_ERROR("unable to get group traffic lights: actor not found");
    }
    if (CarlaActor->IsDormant())
    {
      //todo implement
      return std::vector<cr::ActorId>();
    }
    else
    {
      auto TrafficLight = Cast<ATrafficLightBase>(CarlaActor->GetActor());
      if (TrafficLight == nullptr)
      {
        RESPOND_ERROR("unable to get group traffic lights: actor is not a traffic light");
      }
      std::vector<cr::ActorId> Result;
      for (auto* TLight : TrafficLight->GetGroupTrafficLights())
      {
        auto* View = Episode->FindCarlaActor(TLight);
        if (View)
        {
          Result.push_back(View->GetActorId());
        }
      }
      return Result;
    }
  };

  BIND_SYNC(get_light_boxes) << [this](
      const cr::ActorId ActorId) -> R<std::vector<cg::BoundingBox>>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if (!CarlaActor)
    {
      return RespondError(
          "get_light_boxes",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    if (CarlaActor->IsDormant())
    {
      return RespondError(
          "get_light_boxes",
          ECarlaServerResponse::FunctionNotAvailiableWhenDormant,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    else
    {
      ATrafficLightBase* TrafficLight = Cast<ATrafficLightBase>(CarlaActor->GetActor());
      if (!TrafficLight)
      {
        return RespondError(
          "get_light_boxes",
          ECarlaServerResponse::NotATrafficLight,
          " Actor Id: " + FString::FromInt(ActorId));
      }
      TArray<FBoundingBox> Result;
      TArray<uint8> OutTag;
      UBoundingBoxCalculator::GetTrafficLightBoundingBox(
          TrafficLight, Result, OutTag,
          static_cast<uint8>(carla::rpc::CityObjectLabel::TrafficLight));
      return MakeVectorFromTArray<cg::BoundingBox>(Result);
    }
  };

  // ~~ GBuffer tokens ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  BIND_SYNC(get_gbuffer_token) << [this](const cr::ActorId ActorId, uint32_t GBufferId) -> R<std::vector<unsigned char>>
  {
    REQUIRE_CARLA_EPISODE();
    FCarlaActor* CarlaActor = Episode->FindCarlaActor(ActorId);
    if(!CarlaActor)
    {
      return RespondError(
          "get_gbuffer_token",
          ECarlaServerResponse::ActorNotFound,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    if (CarlaActor->IsDormant())
    {
      return RespondError(
          "get_gbuffer_token",
          ECarlaServerResponse::FunctionNotAvailiableWhenDormant,
          " Actor Id: " + FString::FromInt(ActorId));
    }
    ASceneCaptureSensor* Sensor = Cast<ASceneCaptureSensor>(CarlaActor->GetActor());
    if (!Sensor)
    {
      return RespondError(
        "get_gbuffer_token",
        ECarlaServerResponse::ActorTypeMismatch,
        " Actor Id: " + FString::FromInt(ActorId));
    }

    switch (GBufferId)
    {
      case 0:
      {
        const auto &Token = Sensor->CameraGBuffers.SceneColor.GetToken();
        return std::vector<unsigned char>(std::begin(Token.data), std::end(Token.data));
      }
      case 1:
      {
        const auto &Token = Sensor->CameraGBuffers.SceneDepth.GetToken();
        return std::vector<unsigned char>(std::begin(Token.data), std::end(Token.data));
      }
      case 2:
      {
          const auto& Token = Sensor->CameraGBuffers.SceneStencil.GetToken();
          return std::vector<unsigned char>(std::begin(Token.data), std::end(Token.data));
      }
      case 3:
      {
        const auto &Token = Sensor->CameraGBuffers.GBufferA.GetToken();
        return std::vector<unsigned char>(std::begin(Token.data), std::end(Token.data));
      }
      case 4:
      {
        const auto &Token = Sensor->CameraGBuffers.GBufferB.GetToken();
        return std::vector<unsigned char>(std::begin(Token.data), std::end(Token.data));
      }
      case 5:
      {
        const auto &Token = Sensor->CameraGBuffers.GBufferC.GetToken();
        return std::vector<unsigned char>(std::begin(Token.data), std::end(Token.data));
      }
      case 6:
      {
        const auto &Token = Sensor->CameraGBuffers.GBufferD.GetToken();
        return std::vector<unsigned char>(std::begin(Token.data), std::end(Token.data));
      }
      case 7:
      {
        const auto &Token = Sensor->CameraGBuffers.GBufferE.GetToken();
        return std::vector<unsigned char>(std::begin(Token.data), std::end(Token.data));
      }
      case 8:
      {
        const auto &Token = Sensor->CameraGBuffers.GBufferF.GetToken();
        return std::vector<unsigned char>(std::begin(Token.data), std::end(Token.data));
      }
      case 9:
      {
        const auto &Token = Sensor->CameraGBuffers.Velocity.GetToken();
        return std::vector<unsigned char>(std::begin(Token.data), std::end(Token.data));
      }
      case 10:
      {
        const auto &Token = Sensor->CameraGBuffers.SSAO.GetToken();
        return std::vector<unsigned char>(std::begin(Token.data), std::end(Token.data));
      }
      case 11:
      {
          const auto& Token = Sensor->CameraGBuffers.CustomDepth.GetToken();
          return std::vector<unsigned char>(std::begin(Token.data), std::end(Token.data));
      }
      case 12:
      {
          const auto& Token = Sensor->CameraGBuffers.CustomStencil.GetToken();
          return std::vector<unsigned char>(std::begin(Token.data), std::end(Token.data));
      }
      default:
        UE_LOG(LogCarla, Error, TEXT("Requested invalid GBuffer ID %u"), GBufferId);
        return {};
    }
  };

  // ~~ Logging and playback ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  BIND_SYNC(start_recorder) << [this](std::string name, bool AdditionalData) -> R<std::string>
  {
    REQUIRE_CARLA_EPISODE();
    return R<std::string>(Episode->StartRecorder(name, AdditionalData));
  };

  BIND_SYNC(stop_recorder) << [this]() -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    Episode->GetRecorder()->Stop();
    return R<void>::Success();
  };

  BIND_SYNC(show_recorder_file_info) << [this](
      std::string name,
      bool show_all) -> R<std::string>
  {
    REQUIRE_CARLA_EPISODE();
    return R<std::string>(Episode->GetRecorder()->ShowFileInfo(
        name,
        show_all));
  };

  BIND_SYNC(show_recorder_collisions) << [this](
      std::string name,
      char type1,
      char type2) -> R<std::string>
  {
    REQUIRE_CARLA_EPISODE();
    return R<std::string>(Episode->GetRecorder()->ShowFileCollisions(
        name,
        type1,
        type2));
  };

  BIND_SYNC(show_recorder_actors_blocked) << [this](
      std::string name,
      double min_time,
      double min_distance) -> R<std::string>
  {
    REQUIRE_CARLA_EPISODE();
    return R<std::string>(Episode->GetRecorder()->ShowFileActorsBlocked(
        name,
        min_time,
        min_distance));
  };

  BIND_SYNC(replay_file) << [this](
      std::string name,
      double start,
      double duration,
      uint32_t follow_id,
      bool replay_sensors) -> R<std::string>
  {
    REQUIRE_CARLA_EPISODE();
    return R<std::string>(Episode->GetRecorder()->ReplayFile(
        name,
        start,
        duration,
        follow_id,
        replay_sensors));
  };

  BIND_SYNC(set_replayer_time_factor) << [this](double time_factor) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    Episode->GetRecorder()->SetReplayerTimeFactor(time_factor);
    return R<void>::Success();
  };

  BIND_SYNC(set_replayer_ignore_hero) << [this](bool ignore_hero) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    Episode->GetRecorder()->SetReplayerIgnoreHero(ignore_hero);
    return R<void>::Success();
  };

  BIND_SYNC(set_replayer_ignore_spectator) << [this](bool ignore_spectator) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    Episode->GetRecorder()->SetReplayerIgnoreSpectator(ignore_spectator);
    return R<void>::Success();
  };

  BIND_SYNC(stop_replayer) << [this](bool keep_actors) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    Episode->GetRecorder()->StopReplayer(keep_actors);
    return R<void>::Success();
  };

  // ~~ Draw debug shapes ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  BIND_SYNC(draw_debug_shape) << [this](const cr::DebugShape &shape) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    auto *World = Episode->GetWorld();
    check(World != nullptr);
    FDebugShapeDrawer Drawer(*World);
    Drawer.Draw(shape);
    return R<void>::Success();
  };

  // ~~ Apply commands in batch ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  using C = cr::Command;
  using CR = cr::CommandResponse;
  using ActorId = carla::ActorId;

  auto parse_result = [](ActorId id, const auto &response) {
    return response.HasError() ? CR{response.GetError()} : CR{id};
  };

#define MAKE_RESULT(operation) return parse_result(c.actor, operation);

  auto command_visitor = carla::Functional::MakeRecursiveOverload(
      [=](auto self, const C::SpawnActor &c) -> CR {
        auto result = c.parent.has_value() ?
        spawn_actor_with_parent(
            c.description,
            c.transform,
            *c.parent,
            cr::AttachmentType::Rigid,
            c.socket_name) :
        spawn_actor(c.description, c.transform);
        if (!result.HasError())
        {
          ActorId id = result.Get().id;
          auto set_id = carla::Functional::MakeOverload(
              [](C::SpawnActor &) {},
              [](C::ConsoleCommand &) {},
              [id](auto &s) { s.actor = id; });
          for (auto command : c.do_after)
          {
            boost::variant2::visit(set_id, command.command);
            boost::variant2::visit(self, command.command);
          }
          return id;
        }
        return result.GetError();
      },
      [=](auto, const C::DestroyActor &c) {         MAKE_RESULT(destroy_actor(c.actor)); },
      [=](auto, const C::ApplyVehicleControl &c) {  MAKE_RESULT(apply_control_to_vehicle(c.actor, c.control)); },
      [=](auto, const C::ApplyVehicleAckermannControl &c) {  MAKE_RESULT(apply_ackermann_control_to_vehicle(c.actor, c.control)); },
      [=](auto, const C::ApplyWalkerControl &c) {   MAKE_RESULT(apply_control_to_walker(c.actor, c.control)); },
      [=](auto, const C::ApplyVehiclePhysicsControl &c) {  MAKE_RESULT(apply_physics_control(c.actor, c.physics_control)); },
      [=](auto, const C::ApplyTransform &c) {       MAKE_RESULT(set_actor_transform(c.actor, c.transform)); },
      [=](auto, const C::ApplyTargetVelocity &c) {  MAKE_RESULT(set_actor_target_velocity(c.actor, c.velocity)); },
      [=](auto, const C::ApplyTargetAngularVelocity &c) { MAKE_RESULT(set_actor_target_angular_velocity(c.actor, c.angular_velocity)); },
      [=](auto, const C::ApplyImpulse &c) {         MAKE_RESULT(add_actor_impulse(c.actor, c.impulse)); },
      [=](auto, const C::ApplyForce &c) {           MAKE_RESULT(add_actor_force(c.actor, c.force)); },
      [=](auto, const C::ApplyAngularImpulse &c) {  MAKE_RESULT(add_actor_angular_impulse(c.actor, c.impulse)); },
      [=](auto, const C::ApplyTorque &c) {          MAKE_RESULT(add_actor_torque(c.actor, c.torque)); },
      [=](auto, const C::SetSimulatePhysics &c) {   MAKE_RESULT(set_actor_simulate_physics(c.actor, c.enabled)); },
      [=](auto, const C::SetEnableGravity &c) {   MAKE_RESULT(set_actor_enable_gravity(c.actor, c.enabled)); },
      // TODO: SetAutopilot should be removed. This is the old way to control the vehicles
      [=](auto, const C::SetAutopilot &c) {         MAKE_RESULT(set_actor_autopilot(c.actor, c.enabled)); },
      [=](auto, const C::ShowDebugTelemetry &c) {   MAKE_RESULT(show_vehicle_debug_telemetry(c.actor, c.enabled)); },
      [=](auto, const C::SetVehicleLightState &c) { MAKE_RESULT(set_vehicle_light_state(c.actor, c.light_state)); },
//      [=](auto, const C::OpenVehicleDoor &c) {      MAKE_RESULT(open_vehicle_door(c.actor, c.door_idx)); },
//      [=](auto, const C::CloseVehicleDoor &c) {     MAKE_RESULT(close_vehicle_door(c.actor, c.door_idx)); },
      [=](auto, const C::ApplyWalkerState &c) {     MAKE_RESULT(set_walker_state(c.actor, c.transform, c.speed)); },
      [=](auto, const C::ConsoleCommand& c) -> CR {       return console_command(c.cmd); },
      [=](auto, const C::SetTrafficLightState& c) { MAKE_RESULT(set_traffic_light_state(c.actor, c.traffic_light_state)); },
      [=](auto, const C::ApplyLocation& c)        { MAKE_RESULT(set_actor_location(c.actor, c.location)); }
  );

#undef MAKE_RESULT

  BIND_SYNC(apply_batch) << [=](
      const std::vector<cr::Command> &commands,
      bool do_tick_cue)
  {
    std::vector<CR> result;
    result.reserve(commands.size());
    for (const auto &command : commands)
    {
      result.emplace_back(boost::variant2::visit(command_visitor, command.command));
    }
    if (do_tick_cue)
    {
      tick_cue();
    }
    return result;
  };

  // ~~ Light Subsystem ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  BIND_SYNC(query_lights_state) << [this](std::string client) -> R<std::vector<cr::LightState>>
  {
    REQUIRE_CARLA_EPISODE();
    std::vector<cr::LightState> result;
    auto *World = Episode->GetWorld();
    if(World) {
      UCarlaLightSubsystem* CarlaLightSubsystem = World->GetSubsystem<UCarlaLightSubsystem>();
      result = CarlaLightSubsystem->GetLights(FString(client.c_str()));
    }
    return result;
  };

  BIND_SYNC(update_lights_state) << [this]
    (std::string client, const std::vector<cr::LightState>& lights, bool discard_client) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    auto *World = Episode->GetWorld();
    if(World) {
      UCarlaLightSubsystem* CarlaLightSubsystem = World->GetSubsystem<UCarlaLightSubsystem>();
      CarlaLightSubsystem->SetLights(FString(client.c_str()), lights, discard_client);
    }
    return R<void>::Success();
  };

  BIND_SYNC(update_day_night_cycle) << [this]
    (std::string client, const bool active) -> R<void>
  {
    REQUIRE_CARLA_EPISODE();
    auto *World = Episode->GetWorld();
    if(World) {
      UCarlaLightSubsystem* CarlaLightSubsystem = World->GetSubsystem<UCarlaLightSubsystem>();
      CarlaLightSubsystem->SetDayNightCycle(active);
    }
    return R<void>::Success();
  };


  // ~~ Ray Casting ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  BIND_SYNC(project_point) << [this]
      (cr::Location Location, cr::Vector3D Direction, float SearchDistance)
      -> R<std::pair<bool,cr::LabelledPoint>>
  {
    REQUIRE_CARLA_EPISODE();
    auto *World = Episode->GetWorld();
    constexpr float meter_to_centimeter = 100.0f;
    FVector UELocation = Location;
    ACarlaGameModeBase* GameMode = UCarlaStatics::GetGameMode(Episode->GetWorld());
    ALargeMapManager* LargeMap = GameMode->GetLMManager();
    if (LargeMap)
    {
      UELocation = LargeMap->GlobalToLocalLocation(UELocation);
    }
    return URayTracer::ProjectPoint(UELocation, Direction.ToFVector(),
        meter_to_centimeter * SearchDistance, World);
  };

  BIND_SYNC(cast_ray) << [this]
      (cr::Location StartLocation, cr::Location EndLocation)
      -> R<std::vector<cr::LabelledPoint>>
  {
    REQUIRE_CARLA_EPISODE();
    auto *World = Episode->GetWorld();
    FVector UEStartLocation = StartLocation;
    FVector UEEndLocation = EndLocation;
    ACarlaGameModeBase* GameMode = UCarlaStatics::GetGameMode(Episode->GetWorld());
    ALargeMapManager* LargeMap = GameMode->GetLMManager();
    if (LargeMap)
    {
      UEStartLocation = LargeMap->GlobalToLocalLocation(UEStartLocation);
      UEEndLocation = LargeMap->GlobalToLocalLocation(UEEndLocation);
    }
    return URayTracer::CastRay(StartLocation, EndLocation, World);
  };

}

// =============================================================================
// -- Undef helper macros ------------------------------------------------------
// =============================================================================

#undef BIND_ASYNC
#undef BIND_SYNC
#undef REQUIRE_CARLA_EPISODE
#undef RESPOND_ERROR_FSTRING
#undef RESPOND_ERROR
#undef CARLA_ENSURE_GAME_THREAD

// =============================================================================
// -- FCarlaServer -------------------------------------------------------
// =============================================================================

FCarlaServer::FCarlaServer() : Pimpl(nullptr) {}

FCarlaServer::~FCarlaServer() {
  Stop();
}

FDataMultiStream FCarlaServer::Start(uint16_t RPCPort, uint16_t StreamingPort, uint16_t SecondaryPort)
{
  Pimpl = MakeUnique<FPimpl>(RPCPort, StreamingPort, SecondaryPort);
  StreamingPort = Pimpl->StreamingServer.GetLocalEndpoint().port();
  SecondaryPort = Pimpl->SecondaryServer->GetLocalEndpoint().port();

  UE_LOG(
      LogCarlaServer,
      Log,
      TEXT("Initialized CarlaServer: Ports(rpc=%d, streaming=%d, secondary=%d)"),
      RPCPort,
      StreamingPort,
      SecondaryPort);
  return Pimpl->BroadcastStream;
}

void FCarlaServer::NotifyBeginEpisode(UCarlaEpisode &Episode)
{
  check(Pimpl != nullptr);
  UE_LOG(LogCarlaServer, Log, TEXT("New episode '%s' started"), *Episode.GetMapName());
  Pimpl->Episode = &Episode;
}

void FCarlaServer::NotifyEndEpisode()
{
  check(Pimpl != nullptr);
  Pimpl->Episode = nullptr;
}

void FCarlaServer::AsyncRun(uint32 NumberOfWorkerThreads)
{
  check(Pimpl != nullptr);
  /// @todo Define better the number of threads each server gets.
  int ThreadsPerServer = std::max(2u, NumberOfWorkerThreads / 3u);
  int32_t RPCThreads;
  int32_t StreamingThreads;
  int32_t SecondaryThreads;

  UE_LOG(LogCarla, Log, TEXT("FCommandLine %s"), FCommandLine::Get());

  if(!FParse::Value(FCommandLine::Get(), TEXT("-RPCThreads="), RPCThreads))
  {
    RPCThreads = ThreadsPerServer;
  }
  if(!FParse::Value(FCommandLine::Get(), TEXT("-StreamingThreads="), StreamingThreads))
  {
    StreamingThreads = ThreadsPerServer;
  }
  if(!FParse::Value(FCommandLine::Get(), TEXT("-SecondaryThreads="), SecondaryThreads))
  {
    SecondaryThreads = ThreadsPerServer;
  }

  UE_LOG(LogCarla, Log, TEXT("FCarlaServer AsyncRun %d, RPCThreads %d, StreamingThreads %d, SecondaryThreads %d"),
        NumberOfWorkerThreads, RPCThreads, StreamingThreads, SecondaryThreads);

  Pimpl->Server.AsyncRun(RPCThreads);
  Pimpl->StreamingServer.AsyncRun(StreamingThreads);
  Pimpl->SecondaryServer->AsyncRun(SecondaryThreads);
}

void FCarlaServer::RunSome(uint32 Milliseconds)
{
  TRACE_CPUPROFILER_EVENT_SCOPE_STR(__FUNCTION__);
  Pimpl->Server.SyncRunFor(carla::time_duration::milliseconds(Milliseconds));
}

void FCarlaServer::Tick()
{
  (void)Pimpl->TickCuesReceived.fetch_add(1, std::memory_order_release);
}

bool FCarlaServer::TickCueReceived()
{
  auto k = Pimpl->TickCuesReceived.fetch_sub(1, std::memory_order_acquire);
  bool flag = (k > 0);
  if (!flag)
    (void)Pimpl->TickCuesReceived.fetch_add(1, std::memory_order_release);
  return flag;
}

void FCarlaServer::Stop()
{
  if (Pimpl)
  {
    Pimpl->Server.Stop();
    Pimpl->SecondaryServer->Stop();
  }
}

FDataStream FCarlaServer::OpenStream() const
{
  check(Pimpl != nullptr);
  return Pimpl->StreamingServer.MakeStream();
}

std::shared_ptr<carla::multigpu::Router> FCarlaServer::GetSecondaryServer()
{
  return Pimpl->GetSecondaryServer();
}

carla::streaming::Server &FCarlaServer::GetStreamingServer()
{
  return Pimpl->StreamingServer;
}
