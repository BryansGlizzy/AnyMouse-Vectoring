#ifndef ANYMOUSE_KOVAAKS_OFFSETS_H
#define ANYMOUSE_KOVAAKS_OFFSETS_H

#include <stdint.h>

/*
 * Verified UE4.26 Reflection & Static Offsets for KovaaK's (FPSAimTrainer)
 * Binary: FPSAimTrainer/Binaries/Win64/FPSAimTrainer-Win64-Shipping.exe
 * Build:  3.8.7 (UE4.26 Release, PE ImageBase 0x140000000)
 * Extracted via morluto/rea + Ghidra 12.1.2 static analysis of /Script/GameSkillsTrainer.
 */

#define KOVAAKS_EXE_NAME               "FPSAimTrainer-Win64-Shipping.exe"
#define KOVAAKS_DEFAULT_INSTALL_DIR    "/mnt/iscsi-nvme/SteamLibrary/steamapps/common/FPSAimTrainer"
#define KOVAAKS_STATS_DIR              KOVAAKS_DEFAULT_INSTALL_DIR "/FPSAimTrainer/stats"
#define KOVAAKS_FOV_SENS_CONFIG_PATH   KOVAAKS_DEFAULT_INSTALL_DIR "/FPSAimTrainer/Saved/SaveGames/FovSensConfig.json"

/* PE64 Base & Global Engine Pointers (.data section) */
#define UE4_DEFAULT_IMAGE_BASE         0x140000000ULL
#define UE4_GWORLD_RVA                 0x005533750ULL  /* UWorldProxy GWorld (VA 0x145533750) */
#define UE4_FNAMEPOOL_RVA              0x0053C3340ULL  /* FNamePool NamePoolData (VA 0x1453C3340) */

/* UWorld Offsets */
#define UE4_UWORLD_PERSISTENT_LEVEL    0x0030ULL       /* ULevel* PersistentLevel */
#define UE4_UWORLD_GAMESTATE           0x0120ULL       /* AGameStateBase* (AMetaGameState*) GameState */
#define UE4_UWORLD_GAME_INSTANCE       0x0180ULL       /* UGameInstance* OwningGameInstance */

/* ULevel Offsets */
#define UE4_ULEVEL_ACTORS_TARRAY       0x0098ULL       /* TArray<AActor*> Actors */

/* UGameInstance & UPlayer Offsets */
#define UE4_UGAMEINSTANCE_LOCALPLAYERS 0x0038ULL       /* TArray<ULocalPlayer*> LocalPlayers */
#define UE4_UPLAYER_PLAYERCONTROLLER   0x0030ULL       /* APlayerController* (AMetaPlayerController*) */

/* AController & APlayerController Base Offsets */
#define UE4_ACONTROLLER_PAWN           0x0250ULL       /* APawn* Pawn */
#define UE4_ACONTROLLER_CONTROL_ROT    0x0288ULL       /* FRotator ControlRotation (Pitch, Yaw, Roll in deg) */
#define UE4_APC_ACKNOWLEDGED_PAWN      0x02A0ULL       /* APawn* (AMetaCharacter*) AcknowledgedPawn */
#define UE4_APC_CAMERA_MANAGER         0x02B8ULL       /* APlayerCameraManager* PlayerCameraManager */

/* AMetaPlayerController (/Script/GameSkillsTrainer) Offsets */
#define KOV_PC_REG_META_CHARACTER      0x0570ULL       /* AMetaCharacter* RegisteredMetaCharacter */
#define KOV_PC_AUTOAIM_TARGET_CHAR     0x05A0ULL       /* AMetaCharacter* AutoAimCharacterTarget */
#define KOV_PC_AUTOAIM_TARGET_LOC      0x05A8ULL       /* FVector AutoAimTargetLocation */
#define KOV_PC_DISTANCE_TRAVELED       0x05F4ULL       /* float DistanceTraveled */
#define KOV_PC_USER_YAW_SENS           0x0608ULL       /* float UserYawSensitivity */
#define KOV_PC_USER_PITCH_SENS         0x060CULL       /* float UserPitchSensitivity */
#define KOV_PC_MY_CHARACTER            0x0618ULL       /* AMetaCharacter* MyCharacter */
#define KOV_PC_PAUSE_MENU_LOCATION     0x0678ULL       /* int32_t PauseMenuLocation (0 = unpaused gameplay) */
#define KOV_PC_LAST_ENEMY_ATTACKED     0x06A8ULL       /* AMetaCharacter* LastEnemyAttacked */

/* AMetaGameState (/Script/GameSkillsTrainer) Offsets */
#define KOV_GS_META_SCENARIOS_COMP     0x02A0ULL       /* UObject* MetaScenariosComponent */
#define KOV_GS_LOAD_MAP_NAME           0x02C0ULL       /* FString LoadMapName */
#define KOV_GS_CURRENT_MAP_NAME        0x02D8ULL       /* FString CurrentMapName */
#define KOV_GS_CHARACTERS_TARRAY       0x02F0ULL       /* TArray<AMetaCharacter*> Characters (Player + Bots) */

/* AMetaCharacter (/Script/GameSkillsTrainer) Offsets */
#define KOV_CHAR_MY_PLAYER_CONTROLLER  0x05F0ULL       /* APlayerController* MyPlayerController (non-null for player) */
#define KOV_CHAR_MY_AI_CONTROLLER      0x08C8ULL       /* AAIController* MyAiController (non-null for bots) */
#define KOV_CHAR_FIGHT_START           0x09B8ULL       /* int64_t FightStart (FDateTime ticks) */
#define KOV_CHAR_LAST_TTK              0x09C0ULL       /* int64_t LastTTK (FTimespan ticks) */
#define KOV_CHAR_KILL_COUNT            0x09C8ULL       /* int32_t KillCount (live scenario kills) */
#define KOV_CHAR_DEATH_COUNT           0x09CCULL       /* int32_t DeathCount */
#define KOV_CHAR_KILL_STATISTICS       0x09D0ULL       /* TArray<FKillStat> KillStatistics */
#define KOV_CHAR_TOTAL_OVERSHOTS       0x09E0ULL       /* int32_t TotalOverShots */
#define KOV_CHAR_DAMAGE_DONE           0x0A20ULL       /* float DamageDone */
#define KOV_CHAR_LAST_SCORE            0x0A28ULL       /* float LastScore */
#define KOV_CHAR_DAMAGE_THIS_KILL      0x0A2CULL       /* float DamageDoneThisKill */
#define KOV_CHAR_RELOAD_COUNT          0x0A30ULL       /* int32_t ReloadCount */
#define KOV_CHAR_CHALLENGE_START_TIME  0x0A38ULL       /* int64_t ChallengeStartTime (FDateTime ticks) */
#define KOV_CHAR_LAST_STOP_FIRING_TIME 0x0A74ULL       /* float LastStopFiringTime */
#define KOV_CHAR_LAST_SHOT_FIRED_TIME  0x0A78ULL       /* float LastShotFiredTime */
#define KOV_CHAR_TEAM                  0x0C00ULL       /* int32_t Team */
#define KOV_CHAR_HEALTH                0x0E58ULL       /* float Health (> 0.0f when alive) */

/* APlayerCameraManager Offsets */
#define UE4_ACAMERA_CAMERACACHE        0x1AE0ULL       /* FCameraCacheEntry CameraCachePrivate */
#define UE4_FCAMERACACHE_POV           0x0010ULL       /* FMinimalViewInfo POV */

/* AActor & USceneComponent Offsets */
#define UE4_AACTOR_ROOT_COMPONENT      0x0130ULL       /* USceneComponent* RootComponent */
#define UE4_USCENE_RELATIVE_LOCATION   0x011CULL       /* FVector RelativeLocation (X, Y, Z in cm) */
#define UE4_USCENE_RELATIVE_ROTATION   0x0128ULL       /* FRotator RelativeRotation (Pitch, Yaw, Roll) */
#define UE4_USCENE_COMPONENT_VELOCITY  0x0140ULL       /* FVector ComponentVelocity (X, Y, Z) */

typedef struct {
    float x;
    float y;
    float z;
} ue_fvector_t;

typedef struct {
    float pitch;
    float yaw;
    float roll;
} ue_frotator_t;

typedef struct {
    ue_fvector_t  location;
    ue_frotator_t rotation;
    float         fov;
} ue_minimal_view_info_t;

typedef struct {
    uint64_t data_ptr;
    int32_t  count;
    int32_t  max_count;
} ue_tarray_t;

#endif /* ANYMOUSE_KOVAAKS_OFFSETS_H */
