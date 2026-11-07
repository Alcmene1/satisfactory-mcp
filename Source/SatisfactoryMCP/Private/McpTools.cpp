#include "McpTools.h"

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "CollisionQueryParams.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

#include "FGCharacterPlayer.h"
#include "FGHealthComponent.h"
#include "Buildables/FGBuildable.h"
#include "Buildables/FGBuildableStorage.h"
#include "Buildables/FGBuildableGeneratorFuel.h"
#include "FGInventoryComponent.h"
#include "FGRecipe.h"
#include "ItemAmount.h"
#include "Resources/FGItemDescriptor.h"
#include "Resources/FGBuildDescriptor.h"
#include "Resources/FGResourceDescriptor.h"
#include "Resources/FGResourceNode.h"
#include "FGBuildableSubsystem.h" // FNetConstructionID lives here
#include "FGLightweightBuildableSubsystem.h"
#include "FGGamePhaseManager.h"
#include "Buildables/FGBuildableFactory.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "FGFactoryConnectionComponent.h"
#include "Hologram/FGConveyorAttachmentHologram.h"
#include "Hologram/FGPipelineAttachmentHologram.h"

#include "Hologram/FGHologram.h"
#include "Hologram/FGBuildableHologram.h"
#include "Hologram/FGSplineHologram.h"
#include "Hologram/FGWireHologram.h"
#include "Hologram/FGResourceExtractorHologram.h"
#include "FGConstructDisqualifier.h"

#include <atomic>
#include "Buildables/FGBuildableGenerator.h"
#include "Buildables/FGBuildableWire.h"
#include "Buildables/FGBuildablePowerPole.h"
#include "FGPowerConnectionComponent.h"
#include "FGPowerInfoComponent.h"
#include "UObject/UnrealType.h"

namespace
{
	UWorld* GetGameWorld()
	{
		if (!GEngine)
		{
			return nullptr;
		}
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if ((Ctx.WorldType == EWorldType::Game || Ctx.WorldType == EWorldType::PIE) && Ctx.World())
			{
				return Ctx.World();
			}
		}
		return nullptr;
	}

	bool RunOnGameThread(TFunction<void()> Fn, float TimeoutSec = 15.f)
	{
		auto Done = MakeShared<std::atomic<bool>>(false);
		AsyncTask(ENamedThreads::GameThread, [Fn, Done]()
		{
			Fn();
			Done->store(true);
		});
		const double Start = FPlatformTime::Seconds();
		while (!Done->load() && FPlatformTime::Seconds() - Start < TimeoutSec)
		{
			FPlatformProcess::Sleep(0.002f);
		}
		return Done->load();
	}

	TSharedPtr<FJsonObject> ParseArgs(const FString& ArgsJson)
	{
		if (ArgsJson.IsEmpty())
		{
			return MakeShared<FJsonObject>();
		}
		TSharedPtr<FJsonObject> Obj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgsJson);
		FJsonSerializer::Deserialize(Reader, Obj);
		return Obj.IsValid() ? Obj : MakeShared<FJsonObject>();
	}

	// ---- tool implementations (run on game thread) ----

	FMcpToolResult ToolGetPlayerState()
	{
		UWorld* World = GetGameWorld();
		if (!World)
		{
			return {true, TEXT("{\"error\":\"no active game world\"}")};
		}
		FString Result;
		RunOnGameThread([&World, &Result]()
		{
			APlayerController* PC = World->GetFirstPlayerController();
			APawn* Pawn = PC ? PC->GetPawn() : nullptr;
			if (!Pawn)
			{
				Result = TEXT("{\"error\":\"no player pawn\"}");
				return;
			}
			const FVector Loc = Pawn->GetActorLocation();
			const FRotator Rot = Pawn->GetActorRotation();
			FString HealthJson = TEXT("null");
			if (AFGCharacterPlayer* Char = Cast<AFGCharacterPlayer>(Pawn))
			{
				if (const UFGHealthComponent* HC = Char->GetHealthComponent())
				{
					HealthJson = FString::Printf(TEXT("%.1f"), HC->GetCurrentHealth());
				}
			}
			const FVector Vel = Pawn->GetVelocity();
			Result = FString::Printf(
				TEXT("{\"positionMeters\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f},\"yawDegrees\":%.1f,\"health\":%s,\"velocityCmPerSec\":{\"x\":%.1f,\"y\":%.1f,\"z\":%.1f}}"),
				Loc.X / 100.0, Loc.Y / 100.0, Loc.Z / 100.0, Rot.Yaw, *HealthJson,
				Vel.X, Vel.Y, Vel.Z);
		});
		return Result.IsEmpty() ? FMcpToolResult{true, TEXT("{\"error\":\"game thread timeout\"}")} : FMcpToolResult{false, Result};
	}

	FMcpToolResult ToolListBuildables(const TSharedPtr<FJsonObject>& Args)
	{
		int32 MaxCount = 100;
		Args->TryGetNumberField(TEXT("max"), MaxCount);
		MaxCount = FMath::Clamp(MaxCount, 1, 1000);

		UWorld* World = GetGameWorld();
		if (!World)
		{
			return {true, TEXT("{\"error\":\"no active game world\"}")};
		}

		FString Result;
		RunOnGameThread([&World, MaxCount, &Result]()
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			int32 Total = 0;
			for (TActorIterator<AFGBuildable> It(World); It; ++It)
			{
				++Total;
				if (Items.Num() < MaxCount)
				{
					AFGBuildable* B = *It;
					const FVector Loc = B->GetActorLocation();
					TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("class"), B->GetClass()->GetName());
					Item->SetNumberField(TEXT("x"), Loc.X / 100.0);
					Item->SetNumberField(TEXT("y"), Loc.Y / 100.0);
					Item->SetNumberField(TEXT("z"), Loc.Z / 100.0);
					Item->SetBoolField(TEXT("hidden"), B->IsHidden());
					Items.Add(MakeShared<FJsonValueObject>(Item));
				}
			}
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetNumberField(TEXT("totalBuildables"), Total);
			Root->SetArrayField(TEXT("items"), Items);
			FString Out;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
			FJsonSerializer::Serialize(Root, Writer);
			Result = MoveTemp(Out);
		});
		return Result.IsEmpty() ? FMcpToolResult{true, TEXT("{\"error\":\"game thread timeout\"}")} : FMcpToolResult{false, Result};
	}

	FString ItemName(TSubclassOf<UFGItemDescriptor> Cls)
	{
		return Cls ? UFGItemDescriptor::GetItemName(Cls).ToString() : FString(TEXT("Unknown"));
	}

	TSharedPtr<FJsonObject> StackToJson(const FInventoryStack& S)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("item"), ItemName(S.Item.GetItemClass()));
		J->SetNumberField(TEXT("count"), S.NumItems);
		return J;
	}

	TArray<TSharedPtr<FJsonValue>> StacksToJson(const TArray<FInventoryStack>& Stacks)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FInventoryStack& S : Stacks)
		{
			if (S.HasItems())
			{
				Arr.Add(MakeShared<FJsonValueObject>(StackToJson(S)));
			}
		}
		return Arr;
	}

	FString LowerUnicode(const FString& S)
	{
		// Shipping builds lowercase only ASCII (ToLower and ESearchCase::IgnoreCase alike);
		// do Cyrillic А-Я/Ё by hand, then let ToLower handle the rest.
		FString Out = S;
		for (TCHAR& C : Out)
		{
			if (C >= 0x0410 && C <= 0x042F) { C = static_cast<TCHAR>(C + 0x20); }
			else if (C == 0x0401) { C = 0x0451; }
		}
		return Out.ToLower();
	}

	FString FinishJson(TSharedRef<FJsonObject> Root)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root, W);
		return Out;
	}

	FMcpToolResult ToolBuildablesSummary()
	{
		UWorld* World = GetGameWorld();
		if (!World)
		{
			return {true, TEXT("{\"error\":\"no active game world\"}")};
		}
		FString Result;
		RunOnGameThread([&World, &Result]()
		{
			TMap<FString, int32> Counts;
			int32 Total = 0;
			for (TActorIterator<AFGBuildable> It(World); It; ++It)
			{
				++Total;
				Counts.FindOrAdd((*It)->GetClass()->GetName())++;
			}
			TArray<TPair<FString, int32>> Pairs;
			for (auto& P : Counts)
			{
				Pairs.Add(P);
			}
			Pairs.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B) { return A.Value > B.Value; });
			TArray<TSharedPtr<FJsonValue>> Arr;
			for (const auto& P : Pairs)
			{
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				J->SetStringField(TEXT("class"), P.Key);
				J->SetNumberField(TEXT("count"), P.Value);
				Arr.Add(MakeShared<FJsonValueObject>(J));
			}
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetNumberField(TEXT("totalBuildables"), Total);
			Root->SetArrayField(TEXT("byClass"), Arr);
			Result = FinishJson(Root);
		});
		return Result.IsEmpty() ? FMcpToolResult{true, TEXT("{\"error\":\"game thread timeout\"}")} : FMcpToolResult{false, Result};
	}

	FMcpToolResult ToolFindBuildables(const TSharedPtr<FJsonObject>& Args)
	{
		FString NameFilter;
		Args->TryGetStringField(TEXT("name"), NameFilter);
		NameFilter = NameFilter.ToLower();
		double NX = 0, NY = 0, NZ = 0;
		const bool bNear = Args->TryGetNumberField(TEXT("near_x"), NX)
			&& Args->TryGetNumberField(TEXT("near_y"), NY)
			&& Args->TryGetNumberField(TEXT("near_z"), NZ);
		double Radius = 100.0;
		Args->TryGetNumberField(TEXT("radius_m"), Radius);
		int32 Limit = 50;
		Args->TryGetNumberField(TEXT("limit"), Limit);
		Limit = FMath::Clamp(Limit, 1, 500);

		UWorld* World = GetGameWorld();
		if (!World)
		{
			return {true, TEXT("{\"error\":\"no active game world\"}")};
		}
		FString Result;
		RunOnGameThread([&World, &NameFilter, bNear, NX, NY, NZ, Radius, Limit, &Result]()
		{
			const FVector Center(NX * 100.0, NY * 100.0, NZ * 100.0);
			TArray<TSharedPtr<FJsonValue>> Items;
			int32 Matches = 0;
			for (TActorIterator<AFGBuildable> It(World); It; ++It)
			{
				const FString Cls = (*It)->GetClass()->GetName();
				if (!NameFilter.IsEmpty() && !Cls.Contains(NameFilter, ESearchCase::IgnoreCase))
				{
					continue;
				}
				const FVector Loc = (*It)->GetActorLocation();
				if (bNear && FVector::Dist(Loc, Center) > Radius * 100.0)
				{
					continue;
				}
				++Matches;
				if (Items.Num() < Limit)
				{
					TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
					J->SetStringField(TEXT("class"), Cls);
					J->SetNumberField(TEXT("x"), Loc.X / 100.0);
					J->SetNumberField(TEXT("y"), Loc.Y / 100.0);
					J->SetNumberField(TEXT("z"), Loc.Z / 100.0);
					Items.Add(MakeShared<FJsonValueObject>(J));
				}
			}
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetNumberField(TEXT("totalMatches"), Matches);
			Root->SetArrayField(TEXT("items"), Items);
			Result = FinishJson(Root);
		});
		return Result.IsEmpty() ? FMcpToolResult{true, TEXT("{\"error\":\"game thread timeout\"}")} : FMcpToolResult{false, Result};
	}

	FMcpToolResult ToolPlayerInventory()
	{
		UWorld* World = GetGameWorld();
		if (!World)
		{
			return {true, TEXT("{\"error\":\"no active game world\"}")};
		}
		FString Result;
		RunOnGameThread([&World, &Result]()
		{
			APlayerController* PC = World->GetFirstPlayerController();
			AFGCharacterPlayer* Char = PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
			if (!Char || !Char->GetInventory())
			{
				Result = TEXT("{\"error\":\"no player inventory\"}");
				return;
			}
			TArray<FInventoryStack> Stacks;
			Char->GetInventory()->GetInventoryStacks(Stacks, false);
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetNumberField(TEXT("usedSlots"), Stacks.Num());
			Root->SetArrayField(TEXT("items"), StacksToJson(Stacks));
			Result = FinishJson(Root);
		});
		return Result.IsEmpty() ? FMcpToolResult{true, TEXT("{\"error\":\"game thread timeout\"}")} : FMcpToolResult{false, Result};
	}

	FMcpToolResult ToolListStorages(const TSharedPtr<FJsonObject>& Args)
	{
		int32 Limit = 25;
		Args->TryGetNumberField(TEXT("limit"), Limit);
		Limit = FMath::Clamp(Limit, 1, 200);
		bool bIncludeEmpty = false;
		Args->TryGetBoolField(TEXT("include_empty"), bIncludeEmpty);
		double NX = 0, NY = 0, NZ = 0;
		const bool bNear = Args->TryGetNumberField(TEXT("near_x"), NX)
			&& Args->TryGetNumberField(TEXT("near_y"), NY)
			&& Args->TryGetNumberField(TEXT("near_z"), NZ);
		double Radius = 200.0;
		Args->TryGetNumberField(TEXT("radius_m"), Radius);

		UWorld* World = GetGameWorld();
		if (!World)
		{
			return {true, TEXT("{\"error\":\"no active game world\"}")};
		}
		FString Result;
		RunOnGameThread([&World, Limit, bIncludeEmpty, bNear, NX, NY, NZ, Radius, &Result]()
		{
			const FVector Center(NX * 100.0, NY * 100.0, NZ * 100.0);
			TArray<TSharedPtr<FJsonValue>> Items;
			int32 Total = 0;
			for (TActorIterator<AFGBuildableStorage> It(World); It; ++It)
			{
				const FVector Loc = (*It)->GetActorLocation();
				if (bNear && FVector::Dist(Loc, Center) > Radius * 100.0)
				{
					continue;
				}
				++Total;
				if (Items.Num() >= Limit)
				{
					continue;
				}
				TArray<FInventoryStack> Stacks;
				if (UFGInventoryComponent* Inv = (*It)->GetStorageInventory())
				{
					Inv->GetInventoryStacks(Stacks, false);
				}
				if (!bIncludeEmpty && Stacks.Num() == 0)
				{
					continue;
				}
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				J->SetStringField(TEXT("class"), (*It)->GetClass()->GetName());
				J->SetNumberField(TEXT("x"), Loc.X / 100.0);
				J->SetNumberField(TEXT("y"), Loc.Y / 100.0);
				J->SetNumberField(TEXT("z"), Loc.Z / 100.0);
				J->SetArrayField(TEXT("items"), StacksToJson(Stacks));
				Items.Add(MakeShared<FJsonValueObject>(J));
			}
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetNumberField(TEXT("totalStorages"), Total);
			Root->SetArrayField(TEXT("storages"), Items);
			Result = FinishJson(Root);
		});
		return Result.IsEmpty() ? FMcpToolResult{true, TEXT("{\"error\":\"game thread timeout\"}")} : FMcpToolResult{false, Result};
	}

	FMcpToolResult ToolSetRecipe(const TSharedPtr<FJsonObject>& Args)
	{
		double X = 0, Y = 0, Z = 0;
		if (!Args->TryGetNumberField(TEXT("x"), X) || !Args->TryGetNumberField(TEXT("y"), Y) || !Args->TryGetNumberField(TEXT("z"), Z))
		{
			return {true, TEXT("{\"error\":\"x,y,z (meters) required\"}")};
		}
		FString Product;
		if (!Args->TryGetStringField(TEXT("product"), Product) || Product.IsEmpty())
		{
			return {true, TEXT("{\"error\":\"product (localized substring) required\"}")};
		}
		double Radius = 10.0;
		Args->TryGetNumberField(TEXT("radius_m"), Radius);

		UWorld* World = GetGameWorld();
		if (!World)
		{
			return {true, TEXT("{\"error\":\"no active game world\"}")};
		}
		FString Result;
		RunOnGameThread([&World, X, Y, Z, Radius, &Product, &Result]()
		{
			const FVector C(X * 100.0, Y * 100.0, Z * 100.0);
			AFGBuildableFactory* Mach = nullptr;
			float Best = 1e30f;
			for (TActorIterator<AFGBuildableFactory> It(World); It; ++It)
			{
				const float D = FVector::Dist((*It)->GetActorLocation(), C);
				if (D < Best) { Best = D; Mach = *It; }
			}
			if (!Mach || Best > Radius * 100.0)
			{
				Result = FString::Printf(TEXT("{\"error\":\"no factory within %.0f m\"}"), Radius);
				return;
			}
			AFGBuildableManufacturer* Manu = Cast<AFGBuildableManufacturer>(Mach);
			if (!Manu)
			{
				Result = FString::Printf(TEXT("{\"error\":\"nearest is not a manufacturer\",\"class\":\"%s\"}"), *Mach->GetClass()->GetName());
				return;
			}
			TArray<TSubclassOf<UFGRecipe>> Available;
			Manu->GetAvailableRecipes(Available);
			const FString PF = LowerUnicode(Product);
			FString ChosenName, Candidates;
			TSubclassOf<UFGRecipe> Chosen = nullptr;
			for (TSubclassOf<UFGRecipe> RC : Available)
			{
				const UFGRecipe* RCD0 = RC ? RC.GetDefaultObject() : nullptr;
				if (!RCD0) continue;
				for (const FItemAmount& P : RCD0->GetProducts())
				{
					const FString PN = ItemName(P.ItemClass);
					if (LowerUnicode(PN).Contains(PF))
					{
						Chosen = RC; ChosenName = PN; break;
					}
				}
				if (Chosen) break;
			}
			if (!Chosen)
			{
				for (TSubclassOf<UFGRecipe> RC : Available)
				{
					const UFGRecipe* RCD0 = RC ? RC.GetDefaultObject() : nullptr;
					if (!RCD0) continue;
					for (const FItemAmount& P : RCD0->GetProducts()) { Candidates += ItemName(P.ItemClass) + TEXT("; "); }
				}
				Result = FString::Printf(TEXT("{\"error\":\"product not in machine recipes\",\"available\":[\"%s\"]}"), *Candidates);
				return;
			}
			Manu->SetRecipe(Chosen);
			Result = FString::Printf(TEXT("{\"recipeSet\":true,\"machine\":\"%s\",\"product\":\"%s\"}"), *Mach->GetClass()->GetName(), *ChosenName);
		});
		return Result.IsEmpty() ? FMcpToolResult{true, TEXT("{\"error\":\"game thread timeout\"}")} : FMcpToolResult{false, Result};
	}

	FMcpToolResult ToolGroundZ(const TSharedPtr<FJsonObject>& Args)
	{
		double X = 0, Y = 0;
		if (!Args->TryGetNumberField(TEXT("x"), X) || !Args->TryGetNumberField(TEXT("y"), Y))
		{
			return {true, TEXT("{\"error\":\"x,y (meters) required\"}")};
		}
		UWorld* World = GetGameWorld();
		if (!World)
		{
			return {true, TEXT("{\"error\":\"no active game world\"}")};
		}
		FString Result;
		RunOnGameThread([&World, X, Y, &Result]()
		{
			FHitResult Hit;
			const FVector Start(X * 100.0, Y * 100.0, 50000.0);
			const FVector End(X * 100.0, Y * 100.0, -50000.0);
			FCollisionQueryParams Q(SCENE_QUERY_STAT(McpGroundZ), false);
			if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_WorldStatic, Q))
			{
				Result = FString::Printf(TEXT("{\"groundZ_m\":%.2f}"), Hit.ImpactPoint.Z / 100.0);
			}
			else
			{
				Result = TEXT("{\"groundZ_m\":null}");
			}
		});
		return Result.IsEmpty() ? FMcpToolResult{true, TEXT("{\"error\":\"game thread timeout\"}")} : FMcpToolResult{false, Result};
	}

	FMcpToolResult ToolResourceNodes(const TSharedPtr<FJsonObject>& Args)
	{
		int32 Limit = 60;
		Args->TryGetNumberField(TEXT("limit"), Limit);
		Limit = FMath::Clamp(Limit, 1, 2000);
		double NX = 0, NY = 0, Radius = 0.0;
		bool bNear = Args->TryGetNumberField(TEXT("near_x"), NX) && Args->TryGetNumberField(TEXT("near_y"), NY);
		Args->TryGetNumberField(TEXT("radius_m"), Radius);
		FString ResFilter;
		Args->TryGetStringField(TEXT("resource"), ResFilter);

		UWorld* World = GetGameWorld();
		if (!World)
		{
			return {true, TEXT("{\"error\":\"no active game world\"}")};
		}
		FString Result;
		RunOnGameThread([&World, Limit, bNear, NX, NY, Radius, &ResFilter, &Result]()
		{
			static const TCHAR* PurityNames[] = { TEXT("Impure"), TEXT("Normal"), TEXT("Pure") };
			TArray<TSharedPtr<FJsonValue>> Items;
			int32 Total = 0;
			const FString RF = LowerUnicode(ResFilter);
			for (TActorIterator<AFGResourceNode> It(World); It; ++It)
			{
				const FVector LocPre = (*It)->GetActorLocation();
				if (bNear && Radius > 0.0) { const double ddx = LocPre.X / 100.0 - NX, ddy = LocPre.Y / 100.0 - NY; if (ddx*ddx + ddy*ddy > Radius*Radius) continue; }
				++Total;
				if (Items.Num() >= Limit)
				{
					continue;
				}
				const FString ResName = ItemName((*It)->GetResourceClass());
				if (!RF.IsEmpty() && !LowerUnicode(ResName).Contains(RF)) continue;
				const int32 Purity = (int32)(*It)->GetResourcePurity();
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				J->SetStringField(TEXT("resource"), ItemName((*It)->GetResourceClass()));
				J->SetStringField(TEXT("purity"), Purity >= 0 && Purity < 3 ? PurityNames[Purity] : TEXT("Unknown"));
				const FVector Loc = (*It)->GetActorLocation();
				J->SetNumberField(TEXT("x"), Loc.X / 100.0);
				J->SetNumberField(TEXT("y"), Loc.Y / 100.0);
				J->SetNumberField(TEXT("z"), Loc.Z / 100.0);
				Items.Add(MakeShared<FJsonValueObject>(J));
			}
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetNumberField(TEXT("totalNodes"), Total);
			Root->SetArrayField(TEXT("nodes"), Items);
			Result = FinishJson(Root);
		});
		return Result.IsEmpty() ? FMcpToolResult{true, TEXT("{\"error\":\"game thread timeout\"}")} : FMcpToolResult{false, Result};
	}

	FMcpToolResult ToolListRecipes(const TSharedPtr<FJsonObject>& Args)
	{
		FString Filter;
		Args->TryGetStringField(TEXT("item"), Filter);
		Filter = LowerUnicode(Filter);
		int32 Limit = 40;
		Args->TryGetNumberField(TEXT("limit"), Limit);
		Limit = FMath::Clamp(Limit, 1, 300);
		int32 Mode = 2; // 0=count only, 1=+product names, 2=full with ingredients
		Args->TryGetNumberField(TEXT("mode"), Mode);

		UWorld* World = GetGameWorld();
		if (!World)
		{
			return {true, TEXT("{\"error\":\"no active game world\"}")};
		}
		FString Result;
		RunOnGameThread([&World, &Filter, Limit, Mode, &Result]()
		{
			// NOTE: do NOT go through AFGGameState::GetRecipeManager() - its member layout
			// differs between our headers (CL491125) and the running game (CL513885).
			// Enumerate recipe classes directly instead (engine calls only, ABI-safe).
			auto AmountsToJson = [](const TArray<FItemAmount>& Amounts)
			{
				TArray<TSharedPtr<FJsonValue>> Arr;
				for (const FItemAmount& A : Amounts)
				{
					TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
					J->SetStringField(TEXT("item"), ItemName(A.ItemClass));
					J->SetNumberField(TEXT("count"), A.Amount);
					Arr.Add(MakeShared<FJsonValueObject>(J));
				}
				return Arr;
			};
			TArray<TSharedPtr<FJsonValue>> Items;
			int32 Matches = 0;
			UClass* RecipeBase = UFGRecipe::StaticClass();
			for (TObjectIterator<UClass> It; It; ++It)
			{
				UClass* RClass = *It;
				if (!RClass || RClass == RecipeBase || !RClass->IsChildOf(RecipeBase))
				{
					continue;
				}
				if (RClass->HasAnyClassFlags(EClassFlags::CLASS_Abstract | EClassFlags::CLASS_Deprecated))
				{
					continue;
				}
				const UFGRecipe* CDO = RClass->GetDefaultObject<UFGRecipe>();
				if (!CDO)
				{
					continue;
				}
				FString ProdNamesLower;
				const TArray<FItemAmount>* Products = nullptr;
				if (Mode >= 1)
				{
					Products = &CDO->GetProducts();
					for (const FItemAmount& P : *Products)
					{
						ProdNamesLower += LowerUnicode(ItemName(P.ItemClass)) + TEXT(";");
					}
					if (!Filter.IsEmpty() && !ProdNamesLower.Contains(Filter, ESearchCase::IgnoreCase))
					{
						continue;
					}
				}
				else
				{
					if (!Filter.IsEmpty())
					{
						continue; // filter requires names
					}
				}
				++Matches;
				if (Items.Num() >= Limit)
				{
					continue;
				}
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				J->SetStringField(TEXT("recipe"), RClass ? RClass->GetName() : TEXT("?"));
				if (Mode >= 1 && Products)
				{
					J->SetArrayField(TEXT("products"), AmountsToJson(*Products));
					// Mark buildable recipes: product descriptor carries a hologram class,
					// so the build/build_belt tools can drive it through the game pipeline.
					for (const FItemAmount& P : *Products)
					{
						UClass* PCls = P.ItemClass;
						if (PCls && PCls->IsChildOf(UFGBuildDescriptor::StaticClass()))
						{
							UClass* HC = UFGBuildDescriptor::GetHologramClass(TSubclassOf<UFGBuildDescriptor>(PCls));
							if (HC)
							{
								J->SetBoolField(TEXT("buildable"), true);
								J->SetStringField(TEXT("hologram"), HC->GetName());
								break;
							}
						}
					}
				}
				if (Mode >= 2)
				{
					J->SetArrayField(TEXT("ingredients"), AmountsToJson(CDO->GetIngredients()));
				}
				Items.Add(MakeShared<FJsonValueObject>(J));
			}
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetNumberField(TEXT("totalRecipes"), Matches);
			Root->SetArrayField(TEXT("items"), Items);
			Result = FinishJson(Root);
		});
		return Result.IsEmpty() ? FMcpToolResult{true, TEXT("{\"error\":\"game thread timeout\"}")} : FMcpToolResult{false, Result};
	}

	UClass* FindClassByPattern(UClass* Base, const FString& Pattern, TArray<FString>& OutNearMatches)
	{
		const FString P = LowerUnicode(Pattern);
		UClass* Best = nullptr;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* C = *It;
			if (!C || C == Base || !C->IsChildOf(Base))
			{
				continue;
			}
			if (C->HasAnyClassFlags(EClassFlags::CLASS_Abstract | EClassFlags::CLASS_Deprecated))
			{
				continue;
			}
			const FString CName = LowerUnicode(C->GetName());
			if (CName == P)
			{
				return C; // exact class name wins immediately
			}
			if (CName.Contains(P))
			{
				if (!Best)
				{
					Best = C;
				}
				if (OutNearMatches.Num() < 8)
				{
					OutNearMatches.Add(C->GetName());
				}
			}
		}
		return Best;
	}

	TSubclassOf<UFGItemDescriptor> FindItemDescriptor(const FString& Query, TArray<FString>& OutNearMatches)
	{
		const FString Q = LowerUnicode(Query);
		UClass* Exact = nullptr;
		UClass* Partial = nullptr;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* C = *It;
			if (!C || !C->IsChildOf(UFGItemDescriptor::StaticClass()))
			{
				continue;
			}
			if (C->HasAnyClassFlags(EClassFlags::CLASS_Abstract))
			{
				continue;
			}
			const FString Disp = LowerUnicode(UFGItemDescriptor::GetItemName(C).ToString());
			const FString CName = LowerUnicode(C->GetName());
			if (Disp == Q || CName == Q)
			{
				Exact = C;
				break;
			}
			if (!Partial && (Disp.Contains(Q) || CName.Contains(Q)))
			{
				Partial = C;
			}
			if ((Disp.Contains(Q) || CName.Contains(Q)) && OutNearMatches.Num() < 8)
			{
				OutNearMatches.Add(UFGItemDescriptor::GetItemName(C).ToString());
			}
		}
		return (Exact ? Exact : Partial);
	}

	FMcpToolResult ToolGiveItem(const TSharedPtr<FJsonObject>& Args)
	{
		FString ItemQuery;
		if (!Args->TryGetStringField(TEXT("item"), ItemQuery) || ItemQuery.IsEmpty())
		{
			return {true, TEXT("{\"error\":\"item (display name or class) required\"}")};
		}
		int32 Count = 1;
		Args->TryGetNumberField(TEXT("count"), Count);
		if (Count < 0) { Count = FMath::Clamp(Count, -10000, -1); }
		else { Count = FMath::Clamp(Count, 1, 10000); }
		double NX = 0, NY = 0, NZ = 0;
		const bool bStorage = Args->TryGetNumberField(TEXT("storage_x"), NX)
			&& Args->TryGetNumberField(TEXT("storage_y"), NY)
			&& Args->TryGetNumberField(TEXT("storage_z"), NZ);

		UWorld* World = GetGameWorld();
		if (!World)
		{
			return {true, TEXT("{\"error\":\"no active game world\"}")};
		}
		FString Result;
		RunOnGameThread([&World, &ItemQuery, Count, bStorage, NX, NY, NZ, &Result]()
		{
			TArray<FString> Near;
			TSubclassOf<UFGItemDescriptor> Desc = FindItemDescriptor(ItemQuery, Near);
			if (!Desc)
			{
				FString List;
				for (const FString& N : Near) { List += N + TEXT("; "); }
				Result = FString::Printf(TEXT("{\"error\":\"item not found\",\"nearMatches\":[\"%s\"]}"), *List);
				return;
			}
			UFGInventoryComponent* TargetInv = nullptr;
			auto PropInv=[&](AActor* A,const TCHAR* Name)->UFGInventoryComponent*
			{
				for (TFieldIterator<FObjectProperty> Pit(A->GetClass()); Pit; ++Pit)
				{
					if (FCString::Stricmp(*Pit->GetName(), Name) == 0)
					{
						UObject* V = Pit->GetObjectPropertyValue(A);
						return Cast<UFGInventoryComponent>(V);
					}
				}
				return nullptr;
			};
			if (bStorage)
			{
				const FVector Center(NX * 100.0, NY * 100.0, NZ * 100.0);
				// fuel generators within 15 m win over storages at any distance
				for (TActorIterator<AFGBuildableGeneratorFuel> It(World); It; ++It)
				{
					const float D = FVector::Dist((*It)->GetActorLocation(), Center);
					if (D < 1500.f)
					{
						TargetInv = (*It)->GetFuelInventory();
						if (!TargetInv) TargetInv = PropInv(*It, TEXT("mFuelInventory"));
						if (!TargetInv)
						{
							TArray<UFGInventoryComponent*> Invs;
							(*It)->GetComponents<UFGInventoryComponent>(Invs);
							for (UFGInventoryComponent* I : Invs)
							{
								if (I) { TargetInv = I; break; }
							}
						}
						if (TargetInv) break;
					}
				}
			}
			if (bStorage)
			{
				if (!TargetInv)
				{
					const FVector Center(NX * 100.0, NY * 100.0, NZ * 100.0);
					float BestDist = TNumericLimits<float>::Max();
					for (TActorIterator<AFGBuildableStorage> It(World); It; ++It)
					{
						const float D = FVector::Dist((*It)->GetActorLocation(), Center);
						UFGInventoryComponent* SI = (*It)->GetStorageInventory();
						if (!SI) SI = PropInv(*It, TEXT("mInventory"));
						if (D < BestDist && SI)
						{
							BestDist = D;
							TargetInv = SI;
						}
					}
					if (!TargetInv)
					{
						// generators: fill the fuel inventory when pointing at one
						for (TActorIterator<AFGBuildableGeneratorFuel> It(World); It; ++It)
						{
							const float D = FVector::Dist((*It)->GetActorLocation(), Center);
							if (D < 3000.f && (*It)->GetFuelInventory())
							{
								TargetInv = (*It)->GetFuelInventory();
								break;
							}
						}
					}
					if (!TargetInv)
					{
						APlayerController* PC0 = World->GetFirstPlayerController();
						AFGCharacterPlayer* Ch0 = PC0 ? Cast<AFGCharacterPlayer>(PC0->GetPawn()) : nullptr;
						if (Ch0) TargetInv = Ch0->GetInventory(); // fell back to player inventory, caller can move it via UI/belt
					}
				}
			}
			else
			{
				APlayerController* PC = World->GetFirstPlayerController();
				AFGCharacterPlayer* Char = PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
				TargetInv = Char ? Char->GetInventory() : nullptr;
			}
			if (!TargetInv)
			{
				Result = TEXT("{\"error\":\"no target inventory\"}");
				return;
			}
			const FInventoryItem Item(Desc);
			if (Count < 0)
			{
				const int32 Want = -Count;
				const int32 Have = TargetInv->GetNumItems(Desc);
				const int32 Rem = FMath::Min(Want, Have);
				if (Rem > 0) { TargetInv->Remove(Desc, Rem); }
				Result = FString::Printf(TEXT("{\"item\":\"%s\",\"removed\":%d,\"requested\":%d}"), *UFGItemDescriptor::GetItemName(Desc).ToString(), Rem, Want);
			}
			else
			{
				int32 Added = 0;
				for (int32 i = 0; i < Count; ++i)
				{
					if (!TargetInv->AddItem(Item))
					{
						break;
					}
					++Added;
				}
				const AActor* Tgt = TargetInv->GetOwner();
				const FString TgtCls = Tgt ? Tgt->GetClass()->GetName() : TEXT("?");
				int32 DbgGen = 0, DbgSto = 0;
				FString DbgInv;
				AFGBuildableGeneratorFuel* NearGen = nullptr;
				float NearD = TNumericLimits<float>::Max();
				for (TActorIterator<AFGBuildableGeneratorFuel> It(World); It; ++It) { ++DbgGen; const float D = FVector::Dist((*It)->GetActorLocation(), FVector(NX*100.0,NY*100.0,NZ*100.0)); if (D < NearD) { NearD = D; NearGen = *It; } }
				for (TActorIterator<AFGBuildableStorage> It(World); It; ++It) { ++DbgSto; }
				if (NearGen)
				{
					TArray<UFGInventoryComponent*> Invs;
					NearGen->GetComponents<UFGInventoryComponent>(Invs);
					DbgInv = FString::Printf(TEXT("neargen=%s d=%.1f comps=%d"), *NearGen->GetClass()->GetName(), NearD / 100.0, Invs.Num());
					for (UFGInventoryComponent* I : Invs) DbgInv += FString::Printf(TEXT("|%s"), *GetNameSafe(I));
				}
				Result = FString::Printf(TEXT("{\"item\":\"%s\",\"added\":%d,\"requested\":%d,\"target\":\"%s\",\"bStorage\":%s,\"gens\":%d,\"stor\":%d,\"dbg\":\"%s\"}"), *UFGItemDescriptor::GetItemName(Desc).ToString(), Added, Count, *TgtCls, bStorage ? TEXT("true") : TEXT("false"), DbgGen, DbgSto, *DbgInv);
			}
		});
		return Result.IsEmpty() ? FMcpToolResult{true, TEXT("{\"error\":\"game thread timeout\"}")} : FMcpToolResult{false, Result};
	}

	// =====================================================================
	// Hologram build pipeline - everything is built exactly like the player
	// does it with the build gun: spawn hologram from recipe, feed it hit
	// results (placement/snap/ground logic runs inside game code), step
	// multi-step placement ("clicks"), validate through the game's own
	// rules and finally Construct() - which spawns the real buildable(s)
	// with proper spline geometry, connector snapping, power linking and
	// child support poles.
	// =====================================================================

	FHitResult MakePointHit(const FVector& Pos, AActor* TargetActor, const FVector& Normal)
	{
		// This engine build's FHitResult identifies the hit actor via HitObjectHandle
		// (FActorInstanceHandle); there is no Actor/bStartInside/TraceTag member.
		FHitResult Hit;
		Hit.Init(Pos, Pos);
		Hit.bBlockingHit = true;
		Hit.Time = 0.f;
		Hit.Distance = 0.f;
		Hit.Location = Pos;
		Hit.ImpactPoint = Pos;
		Hit.Normal = Normal.GetSafeNormal();
		Hit.ImpactNormal = Hit.Normal;
		if (TargetActor)
		{
			Hit.HitObjectHandle = FActorInstanceHandle(TargetActor);
		}
		return Hit;
	}

	// Resolve a user query ("smelter", "Конвейер Mk1", "Build_SmelterMk1"...) to a
	// recipe whose product is a build descriptor with a hologram class.
	bool ResolveBuildRecipe(const FString& Query, TSubclassOf<UFGRecipe>& OutRecipe, FString& OutProduct, TArray<FString>& Near)
	{
		const FString Q = LowerUnicode(Query);
		UClass* RecipeBase = UFGRecipe::StaticClass();
		TSubclassOf<UFGRecipe> Partial = nullptr;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* RClass = *It;
			if (!RClass || RClass == RecipeBase || !RClass->IsChildOf(RecipeBase)) continue;
			if (RClass->HasAnyClassFlags(EClassFlags::CLASS_Abstract | EClassFlags::CLASS_Deprecated)) continue;
			const UFGRecipe* CDO = RClass->GetDefaultObject<UFGRecipe>();
			if (!CDO) continue;
			for (const FItemAmount& P : CDO->GetProducts())
			{
				UClass* PCls = P.ItemClass;
				if (!PCls || !PCls->IsChildOf(UFGBuildDescriptor::StaticClass())) continue;
				if (!UFGBuildDescriptor::GetHologramClass(TSubclassOf<UFGBuildDescriptor>(PCls))) continue;
				const FString Disp = LowerUnicode(UFGItemDescriptor::GetItemName(PCls).ToString());
				const FString CName = LowerUnicode(PCls->GetName());
				if (Disp == Q || CName == Q)
				{
					OutRecipe = RClass;
					OutProduct = UFGItemDescriptor::GetItemName(PCls).ToString();
					return true;
				}
				if (Disp.Contains(Q) || CName.Contains(Q))
				{
					if (!Partial) Partial = RClass;
					if (Near.Num() < 8) Near.Add(UFGItemDescriptor::GetItemName(PCls).ToString());
				}
			}
		}
		if (Partial)
		{
			OutRecipe = Partial;
			const UFGRecipe* CDO = Partial->GetDefaultObject<UFGRecipe>();
			OutProduct = (CDO && !CDO->GetProducts().IsEmpty()) ? ItemName(CDO->GetProducts()[0].ItemClass) : FString();
			return true;
		}
		return false;
	}

	FString HoloDisqualifierText(AFGHologram* H)
	{
		TArray<TSubclassOf<UFGConstructDisqualifier>> Disqs;
		H->GetConstructDisqualifiers(Disqs);
		FString Out;
		for (TSubclassOf<UFGConstructDisqualifier> D : Disqs)
		{
			if (!D) continue;
			const FString Name = D->GetName();
			const FString Text = UFGConstructDisqualifier::GetDisqualifyingText(D).ToString();
			if (!Out.IsEmpty()) Out += TEXT("; ");
			Out += Name + (Text.IsEmpty() ? FString() : FString::Printf(TEXT(" (%s)"), *Text));
		}
		return Out;
	}

	// Validate + construct + reconcile build cost against the player inventory.
	// Destroys the hologram on every path. Returns a JSON result string.
	FString FinishHoloBuild(AFGHologram* H, UWorld* World, const FString& Label, bool bDryRun = false)
	{
		if (!H) return FString::Printf(TEXT("{\"error\":\"%s: hologram spawn failed\"}"), *Label);

		APlayerController* PC = World->GetFirstPlayerController();
		AFGCharacterPlayer* Char = PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
		UFGInventoryComponent* Inv = Char ? Char->GetInventory() : nullptr;

		// The game keeps FGCDInitializing on the disqualifier list until the hologram
		// has ticked a few times after spawn. We run synchronously on the game thread,
		// so pump the hologram's own Tick() (public virtual, exported) to fast-forward
		// that initialization instead of waiting for real frames.
		for (int32 t = 0; t < 60; ++t)
		{
			H->ValidatePlacementAndCost(Inv);
			if (H->CanConstruct()) break;
			TArray<TSubclassOf<UFGConstructDisqualifier>> DqNow;
			H->GetConstructDisqualifiers(DqNow);
			bool bInitializing = false;
			for (TSubclassOf<UFGConstructDisqualifier> D : DqNow)
			{
				if (D && (LowerUnicode(D->GetName()).Contains(TEXT("initializing")) || UFGConstructDisqualifier::GetIsSoftDisqualifier(D)))
				{
					bInitializing = true;
					break;
				}
			}
			if (!bInitializing) break;
			if (!H->HasActorBegunPlay())
			{
				H->DispatchBeginPlay();
			}
			H->Tick(0.05f);
		}

		H->ValidatePlacementAndCost(Inv);
		if (bDryRun)
		{
			const FString Dq = HoloDisqualifierText(H);
			// Mirror the real path: init/soft disqualifiers do not block construction.
			bool bValid = H->CanConstruct();
			if (!bValid)
			{
				TArray<TSubclassOf<UFGConstructDisqualifier>> DqDry;
				H->GetConstructDisqualifiers(DqDry);
				bValid = DqDry.Num() > 0;
				for (TSubclassOf<UFGConstructDisqualifier> D : DqDry)
				{
					if (!D || !(LowerUnicode(D->GetName()).Contains(TEXT("initializing")) || UFGConstructDisqualifier::GetIsSoftDisqualifier(D)))
					{
						bValid = false;
						break;
					}
				}
			}
			const FVector HL = H->GetActorLocation();
			H->Destroy();
			return FString::Printf(TEXT("{\"dryRun\":true,\"valid\":%s,\"reasons\":\"%s\",\"at\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}}"),
				bValid ? TEXT("true") : TEXT("false"), *Dq, HL.X / 100.0, HL.Y / 100.0, HL.Z / 100.0);
		}
		if (!H->CanConstruct())
		{
			// FGCDInitializing is UI noise: Construct() succeeds without it cleared
			// (verified live). Soft disqualifiers (yellow clearance warnings) are
			// buildable in vanilla too. Only when EVERY remaining reason is init/soft
			// do we reset the list and construct; genuine hard rejections still fail.
			TArray<TSubclassOf<UFGConstructDisqualifier>> DqSoft;
			H->GetConstructDisqualifiers(DqSoft);
			bool bOnlyInit = DqSoft.Num() > 0;
			for (TSubclassOf<UFGConstructDisqualifier> D : DqSoft)
			{
				if (!D || !(LowerUnicode(D->GetName()).Contains(TEXT("initializing")) || UFGConstructDisqualifier::GetIsSoftDisqualifier(D)))
				{
					bOnlyInit = false;
					break;
				}
			}
			if (bOnlyInit)
			{
				H->ResetConstructDisqualifiers();
				H->SetPlacementMaterialState(EHologramMaterialState::HMS_OK);
			}
			else
			{
				const FString Dq = HoloDisqualifierText(H);
				H->Destroy();
				return FString::Printf(TEXT("{\"error\":\"placement rejected by game rules\",\"holoClass\":\"%s\",\"reasons\":\"%s\"}"),
					*H->GetClass()->GetName(), *Dq);
			}
		}

		// Snapshot cost items before Construct so we can tell whether the game
		// itself deducted them; deduct only any shortfall ourselves afterwards.
		const TArray<FItemAmount> Cost = H->GetCost(true);
		TMap<UClass*, int32> BeforeByClass;
		if (Inv)
		{
			for (const FItemAmount& C : Cost)
			{
				UClass* IC = C.ItemClass;
				if (IC && !BeforeByClass.Contains(IC)) BeforeByClass.Add(IC, Inv->GetNumItems(C.ItemClass));
			}
		}

		TArray<AActor*> Children;
		AActor* Built = nullptr;
		UWorld* BuildWorld = H->GetWorld();
		const FVector HoloPos = H->GetActorLocation();
		auto DoConstructOnce = [&](void) -> AActor*
		{
			// A default FNetConstructionID{-1,0,0} never gets its construction ACK,
			// which can leave spawned buildables in limbo (ghost builds: Construct
			// returns an actor that vanishes right after). Mint a real id like the
			// game's build gun does.
			FNetConstructionID NetID;
			if (AFGBuildableSubsystem* Sub = AFGBuildableSubsystem::Get(H))
				NetID = Sub->GetNewNetConstructionID();
			TArray<AActor*> Kids;
			AActor* B = nullptr;
			if (AFGBuildableHologram* BH = Cast<AFGBuildableHologram>(H))
				B = BH->Construct(Kids, NetID);
			else
				B = H->Construct(Kids, NetID);
			Children.Append(Kids);
			return B;
		};
		auto SeenInWorld = [&](void) -> bool
		{
			for (TActorIterator<AFGBuildable> It(BuildWorld); It; ++It)
			{
				const AFGBuildable* B = *It;
				if (IsValid(B) && FVector::DistSquared2D(B->GetActorLocation(), HoloPos) < 900.f)
					return true;
			}
			// Lightweight classes (foundations, walls...) are stored as instances
			// in the subsystem - no actor is left behind on purpose. Check there too.
			if (AFGLightweightBuildableSubsystem* LW = AFGLightweightBuildableSubsystem::Get(BuildWorld))
			{
				for (const auto& Kv : LW->GetAllLightweightBuildableInstances())
				{
					for (const FRuntimeBuildableInstanceData& D : Kv.Value)
					{
						if (!D.BuiltWithRecipe) continue;
						if (FVector::DistSquared2D(D.Transform.GetTranslation(), HoloPos) < 900.f)
							return true;
					}
				}
			}
			return false;
		};
		Built = DoConstructOnce();
		bool bSeen = SeenInWorld();
		if (!bSeen)
		{
			// ghost retry: construct once more with a fresh net id
			Built = DoConstructOnce();
			bSeen = SeenInWorld();
		}
		const bool bVerified = bSeen || (Built && IsValid(Built));
		H->Destroy();

		if (!bVerified)
		{
			FString Dbg = TEXT("null");
			int32 TotalBuildables = 0;
			for (TActorIterator<AFGBuildable> It(BuildWorld); It; ++It)
				if (IsValid(*It)) ++TotalBuildables;
			if (Built && IsValid(Built))
			{
				const FVector BL = Built->GetActorLocation();
				Dbg = FString::Printf(TEXT("loc=%.1f,%.1f,%.1f root=%d begun=%d"),
					BL.X / 100.0, BL.Y / 100.0, BL.Z / 100.0,
					Built->GetRootComponent() ? 1 : 0, Built->HasActorBegunPlay() ? 1 : 0);
			}
			return FString::Printf(TEXT("{\"error\":\"%s: construct produced no registered actor (ghost)\",\"gotClass\":\"%s\",\"builtInfo\":\"%s\",\"worldBuildables\":%d}"),
				*Label, Built ? *Built->GetClass()->GetName() : TEXT("null"), *Dbg, TotalBuildables);
		}

		int32 Deducted = 0;
		if (Inv)
		{
			for (const FItemAmount& C : Cost)
			{
				UClass* IC = C.ItemClass;
				if (!IC) continue;
				const int32 Before = BeforeByClass.Contains(IC) ? BeforeByClass[IC] : Inv->GetNumItems(C.ItemClass);
				const int32 Now = Inv->GetNumItems(C.ItemClass);
				const int32 Removed = Before - Now;
				if (Removed < C.Amount)
				{
					Inv->Remove(C.ItemClass, C.Amount - Removed);
					Deducted += C.Amount - Removed;
				}
			}
		}

		const bool bHasBuilt = Built && IsValid(Built);
		const FVector Loc = bHasBuilt ? Built->GetActorLocation() : HoloPos;
		const FString Cls = bHasBuilt ? Built->GetClass()->GetName() : TEXT("lightweight-instance");
		return FString::Printf(TEXT("{\"built\":\"%s\",\"at\":{\"x\":%.1f,\"y\":%.1f,\"z\":%.1f},\"yaw\":%.0f,\"children\":%d,\"extraCostDeducted\":%d}"),
			*Cls, Loc.X / 100.0, Loc.Y / 100.0, Loc.Z / 100.0,
			bHasBuilt ? Built->GetActorRotation().Yaw : 0.f, Children.Num(), Deducted);
	}

	FMcpToolResult ToolBuild(const TSharedPtr<FJsonObject>& Args)
	{
		FString Query;
		if (!Args->TryGetStringField(TEXT("query"), Query) || Query.IsEmpty())
		{
			return {true, TEXT("{\"error\":\"query (product display name or class, e.g. 'Smelter' / 'Фундамент') required\"}")};
		}
		double X = 0, Y = 0, Z = 0, Yaw = 0;
		if (!Args->TryGetNumberField(TEXT("x"), X) || !Args->TryGetNumberField(TEXT("y"), Y) || !Args->TryGetNumberField(TEXT("z"), Z))
		{
			return {true, TEXT("{\"error\":\"arguments x,y,z (meters) required\"}")};
		}
		Args->TryGetNumberField(TEXT("yaw"), Yaw);
		double FlatZ = 0;
		const bool bFlat = Args->TryGetNumberField(TEXT("flat_z"), FlatZ);
		bool DryRun = false;
		const bool bDry = Args->TryGetBoolField(TEXT("dry_run"), DryRun);
		// grid: snap x,y to world-grid cell centers (cell size g, centers at k*g + g/2,
		// which is exactly how the game's 8m foundation grid sits around world origin).
		double Grid = 0;
		Args->TryGetNumberField(TEXT("grid"), Grid);
		if (Grid > 0.0)
		{
			const double G = Grid * 100.0;
			X = (FMath::FloorToDouble(X * 100.0 / G) * G + G * 0.5) / 100.0;
			Y = (FMath::FloorToDouble(Y * 100.0 / G) * G + G * 0.5) / 100.0;
		}

		UWorld* World = GetGameWorld();
		if (!World)
		{
			return {true, TEXT("{\"error\":\"no active game world\"}")};
		}
		FString Result;
		RunOnGameThread([&World, &Query, X, Y, Z, Yaw, bFlat, FlatZ, bDry, DryRun, &Result]()
		{
			TSubclassOf<UFGRecipe> Recipe = nullptr;
			FString ProductName;
			TArray<FString> Near;
			if (!ResolveBuildRecipe(Query, Recipe, ProductName, Near))
			{
				FString List;
				for (const FString& N : Near) { List += N + TEXT("; "); }
				Result = FString::Printf(TEXT("{\"error\":\"no buildable recipe matched\",\"nearMatches\":[\"%s\"]}"), *List);
				return;
			}

			APlayerController* PC = World->GetFirstPlayerController();
			AFGCharacterPlayer* Char = PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
			const FVector Target(X * 100.0, Y * 100.0, Z * 100.0);

			AFGHologram* H = AFGHologram::SpawnHologramFromRecipe(Recipe, Char ? (AActor*)Char : (AActor*)World->GetWorldSettings(), Target, Char);
			if (!H)
			{
				Result = FString::Printf(TEXT("{\"error\":\"recipe %s has no hologram or spawn failed\"}"), *Recipe->GetName());
				return;
			}

			// Extractor holograms must snap onto a real resource-node actor: the vanilla
			// build gun hits the node's collider; a plain ground trace fails NeedsResourceNode.
			if (H->IsA(AFGResourceExtractorHologram::StaticClass()))
			{
				AFGResourceNodeBase* BestNode = nullptr;
				double BestD = 1e9;
				for (TObjectIterator<AFGResourceNodeBase> It; It; ++It)
				{
					AFGResourceNodeBase* N = *It;
					if (!N || !IsValid(N)) continue;
					const double D = FVector::DistSquared(N->GetActorLocation(), Target);
					if (D < BestD) { BestD = D; BestNode = N; }
				}
				if (BestNode && BestD < 12000.0 * 12000.0) // within 120 m
				{
					FVector NodeLoc = BestNode->GetActorLocation();
					FHitResult NodeHit;
					bool bRealNodeHit = false;
					// Real trace onto the deposit mesh (like the player's aim ray).
					const FVector NUp = NodeLoc + FVector(0, 0, 2000.0);
					const FVector NDn = NodeLoc - FVector(0, 0, 3000.0);
					FCollisionQueryParams NP(FName(TEXT("McpNodeTrace")));
					for (ECollisionChannel Ch : { ECC_Visibility, ECC_WorldStatic, ECC_GameTraceChannel1 })
					{
						if (World->LineTraceSingleByChannel(NodeHit, NUp, NDn, Ch, NP) && NodeHit.GetActor() == BestNode)
						{
							bRealNodeHit = true;
							break;
						}
					}
					if (!bRealNodeHit)
					{
						NodeLoc.Z += 150.0;
						NodeHit = MakePointHit(NodeLoc, BestNode, FVector::UpVector);
						TArray<UActorComponent*> Comps;
						BestNode->GetComponents(Comps);
						for (UActorComponent* C : Comps)
						{
							if (UPrimitiveComponent* P = Cast<UPrimitiveComponent>(C)) { NodeHit.Component = P; break; }
						}
					}
					H->PreHologramPlacement(NodeHit, false);
					const bool bSnap = H->TrySnapToActor(NodeHit);
					H->SetHologramLocationAndRotation(NodeHit);
					H->UpdateHologramPlacement(NodeHit);
					H->UpdateHologramPlacement(NodeHit);
					const FVector HL = H->GetActorLocation();
					Result = FinishHoloBuild(H, World, TEXT("build"), bDry);
					if (Result.StartsWith(TEXT("{\"built\"")))
					{
						Result = Result.Left(Result.Len() - 1) + FString::Printf(TEXT(",\"product\":\"%s\",\"nodeSnap\":true}"), *ProductName);
					}
					else if (Result.StartsWith(TEXT("{\"error\"")))
					{
						Result = Result.Left(Result.Len() - 1) + FString::Printf(TEXT(",\"nodeSnap\":false,\"snapRet\":%s,\"realHit\":%s,\"node\":\"%s\",\"nodeAt\":{\"x\":%.1f,\"y\":%.1f,\"z\":%.1f},\"holoAt\":{\"x\":%.1f,\"y\":%.1f,\"z\":%.1f}}"),
							bSnap ? TEXT("true") : TEXT("false"), bRealNodeHit ? TEXT("true") : TEXT("false"), *BestNode->GetClass()->GetName(),
							BestNode->GetActorLocation().X / 100.0, BestNode->GetActorLocation().Y / 100.0, BestNode->GetActorLocation().Z / 100.0,
							HL.X / 100.0, HL.Y / 100.0, HL.Z / 100.0);
					}
					return;
				}
			}

			// Prefer a REAL engine trace hit (like the build gun's aim raycast): some
			// blueprint-side init logic only completes when it sees a fully populated
			// FHitResult (component, physics object, sweep data). Fall back to synthetic.
			FHitResult PlaceHit;
			const FVector TraceStart = Target + FVector(0, 0, 3000.0);
			const FVector TraceEnd = Target - FVector(0, 0, 5000.0);
			FCollisionQueryParams Params(FName(TEXT("McpBuildTrace")));
			bool bRealHit = false;
			for (ECollisionChannel Ch : { ECC_Visibility, ECC_WorldStatic, ECC_GameTraceChannel1 })
			{
				if (World->LineTraceSingleByChannel(PlaceHit, TraceStart, TraceEnd, Ch, Params))
				{
					bRealHit = true;
					break;
				}
			}

			// One placement pass: game code does snapping, grid alignment and ground adjust.
			if (bRealHit)
			{
				H->UpdateHologramPlacement(PlaceHit);
			}
			else
			{
				H->UpdateHologramPlacement(MakePointHit(Target, nullptr, FVector::UpVector));
			}

			// Apply the requested yaw through the build gun's own rotation input.
			const float CurYaw = H->GetActorRotation().Yaw;
			const int32 YawDiff = FMath::RoundToInt(FMath::UnwindDegrees(Yaw - CurYaw));
			if (YawDiff != 0)
			{
				H->ScrollRotate(YawDiff, 1);
			}

			// flat_z: force the hologram onto one absolute plane (level floors regardless of terrain).
			if (bFlat)
			{
				const FVector FlatLoc(Target.X, Target.Y, FlatZ * 100.0);
				H->SetHologramLocationAndRotation(MakePointHit(FlatLoc, nullptr, FVector::UpVector));
				H->SetActorLocation(FlatLoc);
			}

			// Conveyor/pipeline attachments (splitters, mergers) must snap onto an existing
			// line: reproduce the build gun's snap so vanilla "must be built on ..." rules pass.
			if (H->IsA(AFGConveyorAttachmentHologram::StaticClass()) || H->IsA(AFGPipelineAttachmentHologram::StaticClass()))
			{
				AFGBuildable* BestLine = nullptr;
				double BestD = 1e9;
				for (AActor* A : World->GetCurrentLevel()->Actors)
				{
					AFGBuildable* B = Cast<AFGBuildable>(A);
					if (!B) continue;
					const FString BName = B->GetClass()->GetName();
					if (!BName.Contains(TEXT("Belt")) && !BName.Contains(TEXT("Pipeline")) && !BName.Contains(TEXT("Pipe"))) continue;
					FVector Sum = FVector::ZeroVector; int32 N = 0;
					TArray<UFGFactoryConnectionComponent*> Comps;
					B->GetComponents(Comps);
					for (UFGFactoryConnectionComponent* C : Comps) { if (C) { Sum += C->GetComponentLocation(); ++N; } }
					if (!N) continue;
					const double D = FVector::DistSquared(Sum / N, Target);
					if (D < BestD) { BestD = D; BestLine = B; }
				}
				if (BestLine && BestD < 3600.0) // within 36 m of a line
				{
					// Hit straight at the middle of the line, carrying one of its real
					// primitive components: IsValidHitResult only accepts conveyor hits.
					FVector LineMid = FVector::ZeroVector; int32 N = 0;
					TArray<UFGFactoryConnectionComponent*> CompsL;
					BestLine->GetComponents(CompsL);
					for (UFGFactoryConnectionComponent* C : CompsL) { if (C) { LineMid += C->GetComponentLocation(); ++N; } }
					LineMid /= N;
					FHitResult SnapHit = MakePointHit(LineMid, BestLine, -FVector::UpVector);
					for (UActorComponent* Cc : BestLine->GetComponents())
					{
						if (UPrimitiveComponent* P = Cast<UPrimitiveComponent>(Cc)) { SnapHit.Component = P; break; }
					}
					H->PreHologramPlacement(SnapHit, false);
					H->TryUpgrade(SnapHit);          // upgrade path: replace line segment with attachment
					H->TrySnapToActor(SnapHit);      // plain snap onto the line
					H->UpdateHologramPlacement(SnapHit);
				}
			}

			Result = FinishHoloBuild(H, World, TEXT("build"), bDry);
			if (Result.StartsWith(TEXT("{\"built\"")))
			{
				Result = Result.Left(Result.Len() - 1) + FString::Printf(TEXT(",\"product\":\"%s\"}"), *ProductName);
			}
		});
		return Result.IsEmpty() ? FMcpToolResult{true, TEXT("{\"error\":\"game thread timeout\"}")} : FMcpToolResult{false, Result};
	}

	UFGFactoryConnectionComponent* FindConnectorNear(UWorld* World, const FVector& P, double RadiusUU, bool bWantOutput, FString& OutInfo)
	{
		UFGFactoryConnectionComponent* Best = nullptr;
		float BestDist = 1e30f;
		for (TActorIterator<AFGBuildable> It(World); It; ++It)
		{
			AFGBuildable* B = *It;
			if (!B) continue;
			TArray<UFGFactoryConnectionComponent*> Comps;
			B->GetComponents<UFGFactoryConnectionComponent>(Comps);
			for (UFGFactoryConnectionComponent* C : Comps)
			{
				if (!C) continue;
				const bool bOut = C->GetDirection() == EFactoryConnectionDirection::FCD_OUTPUT;
				if (bOut != bWantOutput) continue;
				const float D = FVector::Dist(C->GetComponentLocation(), P);
				if (D > RadiusUU || D >= BestDist) continue;
				Best = C;
				BestDist = D;
			}
		}
		if (Best)
		{
			AFGBuildable* Owner = Best->GetOuterBuildable();
			OutInfo = FString::Printf(TEXT("{\"class\":\"%s\",\"distance_m\":%.1f,\"connected\":%s}"),
				Owner ? *Owner->GetClass()->GetName() : TEXT("?"),
				BestDist / 100.0,
				Best->IsConnected() ? TEXT("true") : TEXT("false"));
		}
		return Best;
	}

	FMcpToolResult ToolDismantle(const TSharedPtr<FJsonObject>& Args)
	{
		double X = 0, Y = 0, Z = 0;
		if (!Args->TryGetNumberField(TEXT("x"), X) || !Args->TryGetNumberField(TEXT("y"), Y) || !Args->TryGetNumberField(TEXT("z"), Z))
		{
			return {true, TEXT("{\"error\":\"arguments x,y,z (meters) required\"}")};
		}
		double Radius = 30.0;
		Args->TryGetNumberField(TEXT("radius_m"), Radius);
		FString NameFilter;
		Args->TryGetStringField(TEXT("name"), NameFilter);
		bool bApply = true;
		Args->TryGetBoolField(TEXT("apply"), bApply);

		UWorld* World = GetGameWorld();
		if (!World)
		{
			return {true, TEXT("{\"error\":\"no active game world\"}")};
		}
		FString Result;
		RunOnGameThread([&World, X, Y, Z, Radius, &NameFilter, bApply, &Result]()
		{
			const FVector Center(X * 100.0, Y * 100.0, Z * 100.0);
			const FString F = LowerUnicode(NameFilter);
			AFGBuildable* Target = nullptr;
			float BestDist = TNumericLimits<float>::Max();
			for (TActorIterator<AFGBuildable> It(World); It; ++It)
			{
				AFGBuildable* B = *It;
				if (!B) continue;
				if (!F.IsEmpty() && !LowerUnicode(B->GetClass()->GetName()).Contains(F)) continue;
				const float D = FVector::DistSquared(B->GetActorLocation(), Center);
				if (D > static_cast<float>(Radius * Radius) * 10000.0f) continue;
				if (D < BestDist)
				{
					BestDist = D;
					Target = B;
				}
			}
			if (!Target)
			{
				Result = TEXT("{\"error\":\"no buildable in radius\"}");
				return;
			}
			const FString Cls = Target->GetClass()->GetName();
			if (!bApply)
			{
				Result = FString::Printf(TEXT("{\"target\":\"%s\",\"distance_m\":%.1f,\"apply\":false}"), *Cls, FMath::Sqrt(BestDist) / 100.0);
				return;
			}
			// Interface C++ thunks (CanDismantle/Dismantle) crash due to header/game layout drift;
			// dispatch the BlueprintCallable events via reflection instead (resolved by the game itself).
			UFunction* DismantleFn = Target->FindFunctionChecked(FName(TEXT("Dismantle")));
			if (!DismantleFn)
			{
				Result = FString::Printf(TEXT("{\"error\":\"dismantle function not found\",\"class\":\"%s\"}"), *Cls);
				return;
			}
			Target->ProcessEvent(DismantleFn, nullptr);
			Result = FString::Printf(TEXT("{\"dismantled\":\"%s\"}"), *Cls);
		});
		return Result.IsEmpty() ? FMcpToolResult{true, TEXT("{\"error\":\"game thread timeout\"}")} : FMcpToolResult{false, Result};
	}

	FMcpToolResult ToolBuildMany(const TSharedPtr<FJsonObject>& Args)
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Args->TryGetArrayField(TEXT("items"), Items) || !Items || Items->IsEmpty())
		{
			return {true, TEXT("{\"error\":\"items[] required\"}")};
		}
		FString Results;
		int32 Built = 0, Failed = 0, N = 0;
		for (const TSharedPtr<FJsonValue>& V : *Items)
		{
			const TSharedPtr<FJsonObject>* IO = nullptr;
			if (!V->TryGetObject(IO) || !IO || !(*IO).IsValid()) continue;
			FMcpToolResult R = ToolBuild(*IO);
			++N;
			if (R.Text.Contains(TEXT("\"built\"")) || R.Text.Contains(TEXT("\"valid\":true"))) ++Built; else ++Failed;
			if (!Results.IsEmpty()) Results += TEXT(",");
			Results += R.Text;
		}
		return {false, FString::Printf(TEXT("{\"requested\":%d,\"processed\":%d,\"built\":%d,\"failed\":%d,\"results\":[%s]}"),
			Items->Num(), N, Built, Failed, *Results)};
	}

	FMcpToolResult ToolBuildBelt(const TSharedPtr<FJsonObject>& Args)
	{
		double FX = 0, FY = 0, FZ = 0, TX = 0, TY = 0, TZ = 0;
		if (!Args->TryGetNumberField(TEXT("from_x"), FX) || !Args->TryGetNumberField(TEXT("from_y"), FY) || !Args->TryGetNumberField(TEXT("from_z"), FZ) ||
			!Args->TryGetNumberField(TEXT("to_x"), TX) || !Args->TryGetNumberField(TEXT("to_y"), TY) || !Args->TryGetNumberField(TEXT("to_z"), TZ))
		{
			return {true, TEXT("{\"error\":\"from_x,from_y,from_z,to_x,to_y,to_z (meters) required\"}")};
		}
		double Radius = 6.0;
		Args->TryGetNumberField(TEXT("radius_m"), Radius);
		FString BeltQuery = TEXT("Conveyor Belt Mk1");
		Args->TryGetStringField(TEXT("query"), BeltQuery);
		// mode: "straight" (orthogonal, only 90-degree corners) or "curve" (free spline),
		// mirroring the in-game build-mode toggle. via: intermediate anchor points [m].
		FString BeltMode;
		Args->TryGetStringField(TEXT("mode"), BeltMode);
		int32 MaxSteps = 10;
		Args->TryGetNumberField(TEXT("max_steps"), MaxSteps);
		MaxSteps = FMath::Clamp(MaxSteps, 1, 40);
		bool DryRun = false;
		const bool bDry = Args->TryGetBoolField(TEXT("dry_run"), DryRun);
		TArray<FVector> ViaPts;
		if (const TArray<TSharedPtr<FJsonValue>>* ViaArr = nullptr; Args->TryGetArrayField(TEXT("via"), ViaArr))
		{
			for (const TSharedPtr<FJsonValue>& V : *ViaArr)
			{
				const TSharedPtr<FJsonObject>* VO = nullptr;
				if (!V->TryGetObject(VO) || !VO) continue;
				double vx = 0, vy = 0, vz = 0;
				(*VO)->TryGetNumberField(TEXT("x"), vx);
				(*VO)->TryGetNumberField(TEXT("y"), vy);
				(*VO)->TryGetNumberField(TEXT("z"), vz);
				ViaPts.Add(FVector(vx * 100.0, vy * 100.0, vz * 100.0));
			}
		}

		UWorld* World = GetGameWorld();
		if (!World)
		{
			return {true, TEXT("{\"error\":\"no active game world\"}")};
		}
		FString Result;
		RunOnGameThread([&World, FX, FY, FZ, TX, TY, TZ, Radius, &BeltQuery, &BeltMode, MaxSteps, bDry, DryRun, &ViaPts, &Result]()
		{
			const FVector A(FX * 100.0, FY * 100.0, FZ * 100.0);
			const FVector B(TX * 100.0, TY * 100.0, TZ * 100.0);
			FString AInfo, BInfo;
			UFGFactoryConnectionComponent* OutConn = FindConnectorNear(World, A, Radius * 100.0, true, AInfo);
			UFGFactoryConnectionComponent* InConn = FindConnectorNear(World, B, Radius * 100.0, false, BInfo);
			const bool bFreeEnd = (OutConn != nullptr) && (InConn == nullptr);
			if (!OutConn || (!InConn && !bFreeEnd))
			{
				Result = FString::Printf(TEXT("{\"error\":\"connector not found\",\"fromOutput\":%s,\"toInput\":%s}"),
					OutConn ? *AInfo : TEXT("null"), InConn ? *BInfo : TEXT("null"));
				return;
			}

			TSubclassOf<UFGRecipe> Recipe = nullptr;
			FString ProductName;
			TArray<FString> Near;
			if (!ResolveBuildRecipe(BeltQuery, Recipe, ProductName, Near))
			{
				FString List;
				for (const FString& N : Near) { List += N + TEXT("; "); }
				Result = FString::Printf(TEXT("{\"error\":\"no belt recipe matched\",\"nearMatches\":[\"%s\"]}"), *List);
				return;
			}

			APlayerController* PC = World->GetFirstPlayerController();
			AFGCharacterPlayer* Char = PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;

			const FVector P0 = OutConn->GetComponentLocation();
			AFGHologram* H = AFGHologram::SpawnHologramFromRecipe(Recipe, Char ? (AActor*)Char : (AActor*)World->GetWorldSettings(), P0, Char);
			if (!H)
			{
				Result = FString::Printf(TEXT("{\"error\":\"belt hologram spawn failed for recipe %s\"}"), *Recipe->GetName());
				return;
			}

			const FHitResult HitFrom = MakePointHit(P0, OutConn->GetOuterBuildable(), OutConn->GetForwardVector());
			const FHitResult HitTo = InConn
				? MakePointHit(InConn->GetComponentLocation(), InConn->GetOuterBuildable(), InConn->GetForwardVector())
				: MakePointHit(B, nullptr, FVector(0.f, 0.f, 1.f));

			// Native build modes: read mBuildModeStraight / mBuildModeCurve off the
			// hologram (blueprint classes, names not hardcoded) and override.
			FString ModeNote;
			if (!BeltMode.IsEmpty())
			{
				const bool bCurve = BeltMode.Equals(TEXT("curve"), ESearchCase::IgnoreCase);
				const FName PropName(bCurve ? TEXT("mBuildModeCurve") : TEXT("mBuildModeStraight"));
				UClass* ModeCls = nullptr;
				for (UClass* C = H->GetClass(); C && !ModeCls; C = C->GetSuperClass())
				{
					if (FClassProperty* CP = FindFProperty<FClassProperty>(C, PropName))
					{
						ModeCls = Cast<UClass>(CP->GetObjectPropertyValue_InContainer(H));
					}
				}
				if (ModeCls)
				{
					H->SetBuildModeOverride(ModeCls);
					ModeNote = bCurve ? TEXT("curve") : TEXT("straight");
				}
				else
				{
					ModeNote = FString::Printf(TEXT("mode '%s' unavailable"), *BeltMode);
				}
			}

			// Simulate the player: aim at the output connector (hologram snaps), click,
			// then keep aiming at the input connector and clicking until the spline
			// hologram reports its placement as finished. The game routes the spline
			// (bends, inclines, support poles) and connects both ends in Construct.
			H->UpdateHologramPlacement(HitFrom);
			for (const FVector& VP : ViaPts)
			{
				H->UpdateHologramPlacement(MakePointHit(VP, nullptr, FVector(0.f, 0.f, 1.f)));
				H->DoMultiStepPlacement(true);
			}
			bool bDone = false;
			for (int32 i = 0; i < MaxSteps && !bDone; ++i)
			{
				if (H->DoMultiStepPlacement(true))
				{
					bDone = true;
				}
				else
				{
					H->UpdateHologramPlacement(HitTo);
				}
			}
			if (!bDone)
			{
				const FString Dq = HoloDisqualifierText(H);
				H->Destroy();
				Result = FString::Printf(TEXT("{\"error\":\"belt placement did not complete\",\"reasons\":\"%s\"}"), *Dq);
				return;
			}

			Result = FinishHoloBuild(H, World, TEXT("build_belt"), bDry);
			if (Result.StartsWith(TEXT("{\"built\"")))
			{
				Result = Result.Left(Result.Len() - 1) + FString::Printf(TEXT(",\"product\":\"%s\",\"from\":%s,\"to\":%s%s}"), *ProductName, *AInfo, BInfo.IsEmpty() ? TEXT("null") : *BInfo, ModeNote.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(",\"mode\":\"%s\""), *ModeNote));
			}
		});
		return Result.IsEmpty() ? FMcpToolResult{true, TEXT("{\"error\":\"game thread timeout\"}")} : FMcpToolResult{false, Result};
	}

	UFGPowerConnectionComponent* FindPowerConnectorNear(UWorld* World, const FVector& P, double RadiusUU, FString& OutInfo)
	{
		UFGPowerConnectionComponent* Best = nullptr;
		float BestDist = 1e30f;
		for (TActorIterator<AFGBuildable> It(World); It; ++It)
		{
			AFGBuildable* B = *It;
			if (!IsValid(B)) continue;
			TArray<UFGPowerConnectionComponent*> Comps;
			B->GetComponents<UFGPowerConnectionComponent>(Comps);
			for (UFGPowerConnectionComponent* C : Comps)
			{
				if (!C) continue;
				const float D = FVector::Dist(C->GetComponentLocation(), P);
				if (D > RadiusUU || D >= BestDist) continue;
				Best = C;
				BestDist = D;
			}
		}
		if (Best)
		{
			AActor* Owner = Best->GetOwner();
			OutInfo = FString::Printf(TEXT("{\"class\":\"%s\",\"distance_m\":%.1f}"),
				Owner ? *Owner->GetClass()->GetName() : TEXT("?"), BestDist / 100.0);
		}
		return Best;
	}

	FMcpToolResult ToolWire(const TSharedPtr<FJsonObject>& Args)
	{
		double AX = 0, AY = 0, AZ = 0, BX = 0, BY = 0, BZ = 0, RA = 12.0, RB = 12.0;
		if (!Args->TryGetNumberField(TEXT("x1"), AX) || !Args->TryGetNumberField(TEXT("y1"), AY) || !Args->TryGetNumberField(TEXT("z1"), AZ) ||
			!Args->TryGetNumberField(TEXT("x2"), BX) || !Args->TryGetNumberField(TEXT("y2"), BY) || !Args->TryGetNumberField(TEXT("z2"), BZ))
			return {true, TEXT("{\"error\":\"x1,y1,z1,x2,y2,z2 (meters) required\"}")};
		Args->TryGetNumberField(TEXT("r1"), RA);
		Args->TryGetNumberField(TEXT("r2"), RB);

		UWorld* World = GetGameWorld();
		if (!World) return {true, TEXT("{\"error\":\"no active game world\"}")};
		FString Result;
		RunOnGameThread([&World, AX, AY, AZ, BX, BY, BZ, RA, RB, &Result]()
		{
			const FVector A(AX * 100.0, AY * 100.0, AZ * 100.0);
			const FVector B(BX * 100.0, BY * 100.0, BZ * 100.0);

			auto CircuitOf = [](UFGPowerConnectionComponent* C) -> UObject*
			{
				UObject* Circ = nullptr;
				if (UFunction* F = C->FindFunctionChecked(FName(TEXT("GetPowerCircuit")))) C->ProcessEvent(F, &Circ);
				return Circ;
			};

			FString AInfo, BInfo;
			UFGPowerConnectionComponent* CompA = FindPowerConnectorNear(World, A, RA * 100.0, AInfo);
			UFGPowerConnectionComponent* CompB = FindPowerConnectorNear(World, B, RB * 100.0, BInfo);
			if (!CompA || !CompB)
			{
				Result = FString::Printf(TEXT("{\"error\":\"power connector not found\",\"a\":%s,\"b\":%s}"),
					CompA ? *AInfo : TEXT("null"), CompB ? *BInfo : TEXT("null"));
				return;
			}
			UObject* CircA = CircuitOf(CompA);
			UObject* CircB = CircuitOf(CompB);
			if (CircA && CircA == CircB)
			{
				Result = FString::Printf(TEXT("{\"alreadyConnected\":true,\"a\":%s,\"b\":%s}"), *AInfo, *BInfo);
				return;
			}

			// Wires have no build-menu recipe: vanilla constructs Build_PowerLine and calls
			// its native Connect() between the two circuit connection components. Do exactly
			// that (the wire hologram path needs real physics snaps we cannot fake reliably).
			TArray<FString> NearW;
			UClass* WireCls = FindClassByPattern(AFGBuildableWire::StaticClass(), TEXT("PowerLine"), NearW);
			if (!WireCls)
			{
				// No wire instance ever built in this save -> class not loaded. Force-load
				// the vanilla blueprint (path from save-format docs).
				WireCls = StaticLoadClass(AFGBuildableWire::StaticClass(), nullptr,
					TEXT("/Game/FactoryGame/Buildable/Factory/PowerLine/Build_PowerLine.Build_PowerLine_C"));
			}
			if (!WireCls)
			{
				Result = TEXT("{\"error\":\"wire class not found\"}");
				return;
			}

			const FVector P0 = CompA->GetComponentLocation();
			const FVector P1 = CompB->GetComponentLocation();
			FTransform WT;
			WT.SetTranslation((P0 + P1) * 0.5);
			WT.SetRotation((P1 - P0).GetSafeNormal().ToOrientationQuat());
			AFGBuildableWire* Wire = World->SpawnActorDeferred<AFGBuildableWire>(WireCls, WT, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (!Wire)
			{
				Result = TEXT("{\"error\":\"wire spawn failed\"}");
				return;
			}
			UGameplayStatics::FinishSpawningActor(Wire, WT);
			const bool bConn = Wire->Connect(CompA, CompB);

			UObject* CircA2 = CircuitOf(CompA);
			UObject* CircB2 = CircuitOf(CompB);
			const bool bSame = CircA2 && (CircA2 == CircB2);
			const FVector WL = Wire->GetActorLocation();
			if (!bConn || !bSame)
			{
				// Clean up a dangling wire so we do not litter the world on failure.
				Wire->Destroy();
				Result = FString::Printf(TEXT("{\"error\":\"wire connect failed\",\"connect\":%s,\"sameCircuit\":%s,\"a\":%s,\"b\":%s}"),
					bConn ? TEXT("true") : TEXT("false"), bSame ? TEXT("true") : TEXT("false"), *AInfo, *BInfo);
				return;
			}
			Result = FString::Printf(TEXT("{\"built\":\"Build_PowerLine\",\"at\":{\"x\":%.1f,\"y\":%.1f,\"z\":%.1f},\"sameCircuit\":true,\"a\":%s,\"b\":%s}"),
				WL.X / 100.0, WL.Y / 100.0, WL.Z / 100.0, *AInfo, *BInfo);
		});
		return Result.IsEmpty() ? FMcpToolResult{true, TEXT("{\"error\":\"game thread timeout\"}")} : FMcpToolResult{false, Result};
	}

	FMcpToolResult ToolPowerCheck(const TSharedPtr<FJsonObject>& Args)
	{
		double NX = 0, NY = 0, Radius = 80.0;
		if (!Args->TryGetNumberField(TEXT("near_x"), NX) || !Args->TryGetNumberField(TEXT("near_y"), NY))
			return FMcpToolResult{false, TEXT("{\"error\":\"near_x,near_y required\"}")};
		Args->TryGetNumberField(TEXT("radius_m"), Radius);
		bool bStartArg = false;
		Args->TryGetBoolField(TEXT("start"), bStartArg);
		const double R2 = Radius * 100.0;
		UWorld* World = GetGameWorld();
		if (!World) return FMcpToolResult{false, TEXT("{\"error\":\"no active game world\"}")};
		FString Result;
		RunOnGameThread([&World, NX, NY, R2, bStartArg, &Result]()
		{
			FString Gens, Comps;
			int32 GenN = 0, CompN = 0;
			auto SocketStr = [](AActor* A) -> FString
			{
				FString S;
				if (!A) return S;
				TArray<UFGPowerConnectionComponent*> PCs;
				A->GetComponents<UFGPowerConnectionComponent>(PCs);
				for (UFGPowerConnectionComponent* PC : PCs)
				{
					if (!PC) continue;
					const FVector PL = PC->GetComponentLocation();
					if (!S.IsEmpty()) S += TEXT(",");
					S += FString::Printf(TEXT("{\"px\":%.2f,\"py\":%.2f,\"pz\":%.2f}"), PL.X / 100.0, PL.Y / 100.0, PL.Z / 100.0);
				}
				return S;
			};
			for (TActorIterator<AFGBuildableGenerator> It(World); It; ++It)
			{
				AFGBuildableGenerator* G = *It;
				const FVector L = G->GetActorLocation();
				const double DX = L.X - NX * 100.0, DY = L.Y - NY * 100.0;
				if (DX * DX + DY * DY > R2 * R2) continue;
				bool bCan = false; float Cap = -1.f; bool bHasFuel = false; float FuelAmt = -1.f;
				if (UFunction* F = G->FindFunctionChecked(FName(TEXT("CanStartPowerProduction")))) G->ProcessEvent(F, &bCan);
				if (UFunction* F2 = G->FindFunctionChecked(FName(TEXT("GetPowerProductionCapacity")))) G->ProcessEvent(F2, &Cap);
				if (UFunction* F3 = G->FindFunctionChecked(FName(TEXT("HasFuel")))) G->ProcessEvent(F3, &bHasFuel);
				if (UFunction* F4 = G->FindFunctionChecked(FName(TEXT("GetFuelAmount")))) G->ProcessEvent(F4, &FuelAmt);
				if (bStartArg)
				{
					if (UFunction* F5 = G->FindFunctionChecked(FName(TEXT("Factory_StartPowerProduction")))) G->ProcessEvent(F5, nullptr);
				}
				FString CanStr = bCan ? FString(TEXT("true")) : FString(TEXT("false"));
				FString FuelStr = bHasFuel ? FString(TEXT("true")) : FString(TEXT("false"));
				if (!Gens.IsEmpty()) Gens += TEXT(",");
				Gens += FString::Printf(TEXT("{\"class\":\"%s\",\"x\":%.1f,\"y\":%.1f,\"canStart\":%s,\"capacity\":%.1f,\"hasFuel\":%s,\"fuelAmount\":%.1f,\"sockets\":[%s]}"),
					*G->GetClass()->GetName(), L.X / 100.0, L.Y / 100.0, *CanStr, Cap, *FuelStr, FuelAmt, *SocketStr(G));
				if (++GenN >= 6) break;
			}
			for (TObjectIterator<UFGPowerInfoComponent> It; It; ++It)
			{
				UFGPowerInfoComponent* C = *It;
				if (!C || !IsValid(C)) continue;
				AActor* Ow = C->GetOwner();
				if (!Ow || !IsValid(Ow)) continue;
				const FVector L = Ow->GetActorLocation();
				const double DX = L.X - NX * 100.0, DY = L.Y - NY * 100.0;
				if (DX * DX + DY * DY > R2 * R2) continue;
				float Actual = -1.f, Target = -1.f;
				if (UFunction* F = C->FindFunctionChecked(FName(TEXT("GetActualConsumption")))) C->ProcessEvent(F, &Actual);
				if (UFunction* F2 = C->FindFunctionChecked(FName(TEXT("GetTargetConsumption")))) C->ProcessEvent(F2, &Target);
				UObject* Circ = nullptr;
				if (UFunction* F3 = C->FindFunctionChecked(FName(TEXT("GetPowerCircuit")))) C->ProcessEvent(F3, &Circ);
				if (!Comps.IsEmpty()) Comps += TEXT(",");
				Comps += FString::Printf(TEXT("{\"owner\":\"%s\",\"x\":%.1f,\"y\":%.1f,\"actual\":%.2f,\"target\":%.2f,\"net\":%s,\"sockets\":[%s]}"),
					*Ow->GetClass()->GetName(), L.X / 100.0, L.Y / 100.0, Actual, Target, Circ ? TEXT("true") : TEXT("false"), *SocketStr(Ow));
				if (++CompN >= 30) break;
			}
			Result = FString::Printf(TEXT("{\"generators\":[%s],\"powerConsumers\":[%s]}"), *Gens, *Comps);
		});
		return Result.IsEmpty() ? FMcpToolResult{true, TEXT("{\"error\":\"game thread timeout\"}")} : FMcpToolResult{false, Result};
	}

	FMcpToolResult ToolMachineStatus(const TSharedPtr<FJsonObject>& Args)
	{
		double NX = 0, NY = 0, NZ = 0;
		if (!Args->TryGetNumberField(TEXT("near_x"), NX) || !Args->TryGetNumberField(TEXT("near_y"), NY) || !Args->TryGetNumberField(TEXT("near_z"), NZ))
		{
			return {true, TEXT("{\"error\":\"near_x,near_y,near_z (meters) required\"}")};
		}
		double Radius = 50.0;
		Args->TryGetNumberField(TEXT("radius_m"), Radius);
		FString ClassFilter;
		Args->TryGetStringField(TEXT("class"), ClassFilter);
		int32 Limit = 15;
		Args->TryGetNumberField(TEXT("limit"), Limit);
		Limit = FMath::Clamp(Limit, 1, 100);

		UWorld* World = GetGameWorld();
		if (!World)
		{
			return {true, TEXT("{\"error\":\"no active game world\"}")};
		}
		FString Result;
		RunOnGameThread([&World, NX, NY, NZ, Radius, &ClassFilter, Limit, &Result]()
		{
			const FVector Center(NX * 100.0, NY * 100.0, NZ * 100.0);
			const FString F = LowerUnicode(ClassFilter);
			TArray<TSharedPtr<FJsonValue>> Machines;
			int32 Total = 0;
			for (TActorIterator<AFGBuildableFactory> It(World); It; ++It)
			{
				AFGBuildableFactory* M = *It;
				if (!M) continue;
				const FVector Loc = M->GetActorLocation();
				if (FVector::Dist(Loc, Center) > Radius * 100.0) continue;
				const FString Cls = M->GetClass()->GetName();
				if (!F.IsEmpty() && !LowerUnicode(Cls).Contains(F)) continue;
				++Total;
				if (Machines.Num() >= Limit) continue;

				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				J->SetStringField(TEXT("class"), Cls);
				J->SetNumberField(TEXT("x"), Loc.X / 100.0);
				J->SetNumberField(TEXT("y"), Loc.Y / 100.0);
				J->SetNumberField(TEXT("z"), Loc.Z / 100.0);

				if (AFGBuildableManufacturer* Manu = Cast<AFGBuildableManufacturer>(M))
				{
					TSubclassOf<UFGRecipe> Recipe = Manu->GetCurrentRecipe();
					const UFGRecipe* RCDO = Recipe ? Recipe.GetDefaultObject() : nullptr;
					if (RCDO)
					{
						TArray<TSharedPtr<FJsonValue>> Prods;
						for (const FItemAmount& P : RCDO->GetProducts())
						{
							TSharedRef<FJsonObject> PJ = MakeShared<FJsonObject>();
							PJ->SetStringField(TEXT("item"), ItemName(P.ItemClass));
							PJ->SetNumberField(TEXT("perCycle"), P.Amount);
							Prods.Add(MakeShared<FJsonValueObject>(PJ));
						}
						J->SetArrayField(TEXT("recipeProducts"), Prods);
					}
					else
					{
						J->SetStringField(TEXT("recipeProducts"), TEXT("none"));
					}
				}

				TArray<TSharedPtr<FJsonValue>> Conns;
				TArray<UFGFactoryConnectionComponent*> CompArray;
				M->GetComponents<UFGFactoryConnectionComponent>(CompArray);
				for (UFGFactoryConnectionComponent* C : CompArray)
				{
					if (!C) continue;
					TSharedRef<FJsonObject> CJ = MakeShared<FJsonObject>();
					CJ->SetStringField(TEXT("dir"), C->GetDirection() == EFactoryConnectionDirection::FCD_INPUT ? TEXT("in") : TEXT("out"));
			CJ->SetNumberField(TEXT("cx"), C->GetComponentLocation().X / 100.0);
			CJ->SetNumberField(TEXT("cy"), C->GetComponentLocation().Y / 100.0);
			CJ->SetNumberField(TEXT("cz"), C->GetComponentLocation().Z / 100.0);
					CJ->SetBoolField(TEXT("connected"), C->IsConnected());
					if (C->GetDirection() == EFactoryConnectionDirection::FCD_OUTPUT)
					{
						TArray<FInventoryItem> Peeked;
						if (C->Factory_PeekOutput(Peeked) && Peeked.Num() > 0)
						{
							CJ->SetStringField(TEXT("carries"), ItemName(Peeked[0].GetItemClass()));
						}
					}
					Conns.Add(MakeShared<FJsonValueObject>(CJ));
				}
				J->SetArrayField(TEXT("connections"), Conns);

				TArray<TSharedPtr<FJsonValue>> Power;
				TArray<UFGPowerConnectionComponent*> PComps;
				M->GetComponents<UFGPowerConnectionComponent>(PComps);
				for (UFGPowerConnectionComponent* PC : PComps)
				{
					if (!PC) continue;
					TSharedRef<FJsonObject> PJ = MakeShared<FJsonObject>();
					const FVector PL = PC->GetComponentLocation();
					PJ->SetNumberField(TEXT("px"), PL.X / 100.0);
					PJ->SetNumberField(TEXT("py"), PL.Y / 100.0);
					PJ->SetNumberField(TEXT("pz"), PL.Z / 100.0);
					Power.Add(MakeShared<FJsonValueObject>(PJ));
				}
				J->SetArrayField(TEXT("powerSockets"), Power);
				Machines.Add(MakeShared<FJsonValueObject>(J));
			}
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetNumberField(TEXT("totalMachines"), Total);
			Root->SetArrayField(TEXT("machines"), Machines);
			Result = FinishJson(Root);
		});
		return Result.IsEmpty() ? FMcpToolResult{true, TEXT("{\"error\":\"game thread timeout\"}")} : FMcpToolResult{false, Result};
	}

	FMcpToolResult ToolConstructTest(const TSharedPtr<FJsonObject>& Args)
	{
		FString Query;
		if (!Args->TryGetStringField(TEXT("query"), Query) || Query.IsEmpty())
			return {true, TEXT("{\"error\":\"query required\"}")};
		double X = 0, Y = 0, Z = 0;
		if (!Args->TryGetNumberField(TEXT("x"), X) || !Args->TryGetNumberField(TEXT("y"), Y) || !Args->TryGetNumberField(TEXT("z"), Z))
			return {true, TEXT("{\"error\":\"x,y,z required\"}")};

		UWorld* World = GetGameWorld();
		if (!World) return {true, TEXT("{\"error\":\"no active game world\"}")};
		FString Result;
		RunOnGameThread([&World, &Query, X, Y, Z, &Result]()
		{
			TSubclassOf<UFGRecipe> Recipe = nullptr;
			FString ProductName;
			TArray<FString> Near;
			if (!ResolveBuildRecipe(Query, Recipe, ProductName, Near))
			{
				Result = TEXT("{\"error\":\"no recipe\"}");
				return;
			}
			APlayerController* PC = World->GetFirstPlayerController();
			AFGCharacterPlayer* Char = PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
			const FVector Target(X * 100.0, Y * 100.0, Z * 100.0);
			AFGHologram* H = AFGHologram::SpawnHologramFromRecipe(Recipe, Char ? (AActor*)Char : (AActor*)World->GetWorldSettings(), Target, Char);
			if (!H) { Result = TEXT("{\"error\":\"holo spawn failed\"}"); return; }

			FHitResult PlaceHit;
			FCollisionQueryParams Params(FName(TEXT("McpCtTrace")));
			bool bReal = World->LineTraceSingleByChannel(PlaceHit, Target + FVector(0, 0, 3000.0), Target - FVector(0, 0, 5000.0), ECC_Visibility, Params);
			H->UpdateHologramPlacement(bReal ? PlaceHit : MakePointHit(Target, nullptr, FVector::UpVector));

			const bool bCanBefore = H->CanConstruct();
			TArray<TSubclassOf<UFGConstructDisqualifier>> Dq;
			H->GetConstructDisqualifiers(Dq);
			FString DqList;
			for (auto& D : Dq) { if (!D) continue; const UFGConstructDisqualifier* C = D->GetDefaultObject<UFGConstructDisqualifier>(); DqList += D->GetName() + (C && CastField<FBoolProperty>(C->GetClass()->FindPropertyByName(FName(TEXT("mIsSoftDisqualifier")))) ? TEXT("") : TEXT("")); DqList += TEXT(";"); }

			H->ResetConstructDisqualifiers();
			H->SetPlacementMaterialState(EHologramMaterialState::HMS_OK);
			const bool bCanAfter = H->CanConstruct();

			AActor* Built = nullptr;
			FString Err;
			TArray<AActor*> Children;
			// Wrap: Construct may assert/crash if truly invalid; we accept the risk deliberately (throwaway test).
			if (AFGBuildableHologram* BH = Cast<AFGBuildableHologram>(H))
				Built = BH->Construct(Children, FNetConstructionID());
			else
				Built = H->Construct(Children, FNetConstructionID());
			H->Destroy();
			if (Built)
			{
				const FVector L = Built->GetActorLocation();
				Result = FString::Printf(TEXT("{\"constructed\":\"%s\",\"at\":{\"x\":%.1f,\"y\":%.1f,\"z\":%.1f},\"canBefore\":%s,\"canAfterReset\":%s,\"dqWas\":\"%s\"}"),
					*Built->GetClass()->GetName(), L.X / 100.0, L.Y / 100.0, L.Z / 100.0,
					bCanBefore ? TEXT("true") : TEXT("false"), bCanAfter ? TEXT("true") : TEXT("false"), *DqList);
			}
			else
			{
				Result = FString::Printf(TEXT("{\"constructed\":null,\"canBefore\":%s,\"canAfterReset\":%s,\"dqWas\":\"%s\"}"),
					bCanBefore ? TEXT("true") : TEXT("false"), bCanAfter ? TEXT("true") : TEXT("false"), *DqList);
			}
		});
		return Result.IsEmpty() ? FMcpToolResult{true, TEXT("{\"error\":\"game thread timeout\"}")} : FMcpToolResult{false, Result};
	}

	// Convert lightweight (instanced) buildables - foundations especially - into
	// real registered actors within a radius, using the game's own converter
	// subsystem (same mechanism that triggers when a player walks up to them).
	FMcpToolResult ToolLightweightConvert(const TSharedPtr<FJsonObject>& Args)
	{
		double X = 0, Y = 0, Z = 0, R = 15;
		if (!Args->TryGetNumberField(TEXT("x"), X) || !Args->TryGetNumberField(TEXT("y"), Y) || !Args->TryGetNumberField(TEXT("z"), Z))
		{
			return {true, TEXT("{\"error\":\"arguments x,y,z (meters) required\"}")};
		}
		Args->TryGetNumberField(TEXT("radius_m"), R);

		UWorld* World = GetGameWorld();
		if (!World)
		{
			return {true, TEXT("{\"error\":\"no active game world\"}")};
		}

		FString Result;
		RunOnGameThread([&World, X, Y, Z, R, &Result]()
		{
			AFGLightweightBuildableSubsystem* LW = AFGLightweightBuildableSubsystem::Get(World);
			if (!LW)
			{
				Result = TEXT("{\"error\":\"no lightweight subsystem\"}");
				return;
			}
			const FVector Target(X * 100.0, Y * 100.0, Z * 100.0);
			AActor* A = LW->AddInstanceConverterInstigator((float)(R * 100.0), nullptr, FTransform(Target));
			Result = A ? TEXT("{\"converter\":true}") : TEXT("{\"error\":\"converter not added\"}");
		});
		return Result.IsEmpty() ? FMcpToolResult{true, TEXT("{\"error\":\"game thread timeout\"}")} : FMcpToolResult{false, Result};
	}

	// List lightweight (instanced) buildables - foundations/walls/etc stored without actors.
	FMcpToolResult ToolLightweightList(const TSharedPtr<FJsonObject>& Args)
	{
		double X = 0, Y = 0, Z = 0, R = 50;
		if (!Args->TryGetNumberField(TEXT("x"), X) || !Args->TryGetNumberField(TEXT("y"), Y))
		{
			return {true, TEXT("{\"error\":\"arguments x,y required\"}")};
		}
		Args->TryGetNumberField(TEXT("z"), Z);
		Args->TryGetNumberField(TEXT("radius_m"), R);
		UWorld* World = GetGameWorld();
		if (!World)
		{
			return {true, TEXT("{\"error\":\"no active game world\"}")};
		}
		FString Result;
		RunOnGameThread([&World, X, Y, Z, R, &Result]()
		{
			AFGLightweightBuildableSubsystem* LW = AFGLightweightBuildableSubsystem::Get(World);
			if (!LW)
			{
				Result = TEXT("{\"error\":\"no lightweight subsystem\"}");
				return;
			}
			const FVector Target(X * 100.0, Y * 100.0, Z * 100.0);
			const double R2 = R * 100.0 * (R * 100.0);
			FString Items;
			int32 Total = 0;
			for (const auto& Kv : LW->GetAllLightweightBuildableInstances())
			{
				const FString Cls = Kv.Key ? Kv.Key->GetName() : TEXT("?");
				for (int32 i = 0; i < Kv.Value.Num(); ++i)
				{
					const FRuntimeBuildableInstanceData& D = Kv.Value[i];
					if (!D.BuiltWithRecipe) continue;
					const FVector L = D.Transform.GetTranslation();
					if (FVector::DistSquared(L, Target) > R2) continue;
					++Total;
					const FRotator Rot = D.Transform.Rotator();
					if (Items.Len() > 0) Items += TEXT(",");
					Items += FString::Printf(TEXT("{\"cls\":\"%s\",\"idx\":%d,\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,\"yaw\":%.0f}"),
						*Cls, i, L.X / 100.0, L.Y / 100.0, L.Z / 100.0, Rot.Yaw);
				}
			}
			Result = FString::Printf(TEXT("{\"total\":%d,\"items\":[%s]}"), Total, *Items);
		});
		return Result.IsEmpty() ? FMcpToolResult{true, TEXT("{\"error\":\"game thread timeout\"}")} : FMcpToolResult{false, Result};
	}

	// Remove lightweight instances within radius (cleanup of misplaced lightweights).
	FMcpToolResult ToolLightweightRemove(const TSharedPtr<FJsonObject>& Args)
	{
		double X = 0, Y = 0, Z = 0, R = 50;
		if (!Args->TryGetNumberField(TEXT("x"), X) || !Args->TryGetNumberField(TEXT("y"), Y))
		{
			return {true, TEXT("{\"error\":\"arguments x,y required\"}")};
		}
		Args->TryGetNumberField(TEXT("z"), Z);
		Args->TryGetNumberField(TEXT("radius_m"), R);
		UWorld* World = GetGameWorld();
		if (!World)
		{
			return {true, TEXT("{\"error\":\"no active game world\"}")};
		}
		FString Result;
		RunOnGameThread([&World, X, Y, Z, R, &Result]()
		{
			AFGLightweightBuildableSubsystem* LW = AFGLightweightBuildableSubsystem::Get(World);
			if (!LW)
			{
				Result = TEXT("{\"error\":\"no lightweight subsystem\"}");
				return;
			}
			const FVector Target(X * 100.0, Y * 100.0, Z * 100.0);
			const double R2 = R * 100.0 * (R * 100.0);
			int32 Removed = 0;
			TArray<TSubclassOf<AFGBuildable>> Keys;
			for (const auto& Kv : LW->GetAllLightweightBuildableInstances()) Keys.Add(Kv.Key);
			for (TSubclassOf<AFGBuildable> Cls : Keys)
			{
				const TArray<FRuntimeBuildableInstanceData>* Arr = nullptr;
				for (const auto& Kv : LW->GetAllLightweightBuildableInstances())
					if (Kv.Key == Cls) { Arr = &Kv.Value; break; }
				if (!Arr) continue;
				TArray<int32> Hits;
				for (int32 i = 0; i < Arr->Num(); ++i)
				{
					const FRuntimeBuildableInstanceData& D = (*Arr)[i];
					if (!D.BuiltWithRecipe) continue;
					if (FVector::DistSquared(D.Transform.GetTranslation(), Target) <= R2) Hits.Add(i);
				}
				for (int32 i = Hits.Num() - 1; i >= 0; --i)
				{
					LW->InvalidateRuntimeInstanceDataForIndex(Cls, Hits[i]);
					LW->RemoveByInstanceIndex(Cls, Hits[i]);
					++Removed;
				}
			}
			Result = FString::Printf(TEXT("{\"removed\":%d}"), Removed);
		});
		return Result.IsEmpty() ? FMcpToolResult{true, TEXT("{\"error\":\"game thread timeout\"}")} : FMcpToolResult{false, Result};
	}

	FMcpToolResult ToolTeleportPlayer(const TSharedPtr<FJsonObject>& Args)
	{
		double X = 0, Y = 0, Z = 0;
		if (!Args->TryGetNumberField(TEXT("x"), X) || !Args->TryGetNumberField(TEXT("y"), Y) || !Args->TryGetNumberField(TEXT("z"), Z))
		{
			return {true, TEXT("{\"error\":\"arguments x,y,z (meters) required\"}")};
		}

		UWorld* World = GetGameWorld();
		if (!World)
		{
			return {true, TEXT("{\"error\":\"no active game world\"}")};
		}

		FString Result;
		RunOnGameThread([&World, X, Y, Z, &Result]()
		{
			APlayerController* PC = World->GetFirstPlayerController();
			APawn* Pawn = PC ? PC->GetPawn() : nullptr;
			if (!Pawn)
			{
				Result = TEXT("{\"error\":\"no player pawn\"}");
				return;
			}
			const FVector Target(X * 100.0, Y * 100.0, Z * 100.0);
			FHitResult Hit;
			const bool bOk = Pawn->K2_SetActorLocation(Target, false, Hit, true);
			Result = bOk ? TEXT("{\"ok\":true}") : TEXT("{\"error\":\"move blocked\"}");
		});
		return Result.IsEmpty() ? FMcpToolResult{true, TEXT("{\"error\":\"game thread timeout\"}")} : FMcpToolResult{false, Result};
	}
}

FMcpToolResult ToolSpawnBuildable(const TSharedPtr<FJsonObject>& Args)
{
	FString Query;
	if (!Args->TryGetStringField(TEXT("query"), Query) || Query.IsEmpty())
		return {true, TEXT("{\"error\":\"query (class name substring, e.g. Build_Hub_C) required\"}")};
	double X = 0, Y = 0, Z = 0, Yaw = 0;
	Args->TryGetNumberField(TEXT("x"), X);
	Args->TryGetNumberField(TEXT("y"), Y);
	Args->TryGetNumberField(TEXT("z"), Z);
	Args->TryGetNumberField(TEXT("yaw"), Yaw);

	UWorld* World = GetGameWorld();
	if (!World) return {true, TEXT("{\"error\":\"no active game world\"}")};
	FString Result;
	RunOnGameThread([&World, Query, X, Y, Z, Yaw, &Result]()
	{
		TArray<FString> Near;
		UClass* Cls = FindClassByPattern(AFGBuildable::StaticClass(), Query, Near);
		if (!Cls)
		{
			FString NearJ;
			for (int32 i = 0; i < Near.Num(); ++i)
			{
				if (i) NearJ += TEXT(",");
				NearJ += FString::Printf(TEXT("\"%s\""), *Near[i]);
			}
			Result = FString::Printf(TEXT("{\"error\":\"buildable class not found for '%s'\",\"near\":[%s]}"), *Query, *NearJ);
			return;
		}
		FTransform T(FRotator(0.0, Yaw, 0.0), FVector(X * 100.0, Y * 100.0, Z * 100.0));
		AFGBuildableSubsystem* BS = AFGBuildableSubsystem::Get(World);
		AFGBuildable* B = nullptr;
		if (BS)
		{
			B = BS->BeginSpawnBuildable(Cls, T);
			if (B) UGameplayStatics::FinishSpawningActor(B, T);
		}
		if (!B)
		{
			B = World->SpawnActorDeferred<AFGBuildable>(Cls, T, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (B) UGameplayStatics::FinishSpawningActor(B, T);
		}
		const FVector L = B->GetActorLocation();
		Result = FString::Printf(TEXT("{\"spawned\":\"%s\",\"at\":{\"x\":%.1f,\"y\":%.1f,\"z\":%.1f},\"root\":%s}"),
			*B->GetClass()->GetName(), L.X / 100.0, L.Y / 100.0, L.Z / 100.0,
			B->GetRootComponent() ? TEXT("true") : TEXT("false"));
	});
	return Result.IsEmpty() ? FMcpToolResult{true, TEXT("{\"error\":\"game thread timeout\"}")} : FMcpToolResult{false, Result};
}

FMcpToolResult ToolMoveBuildable(const TSharedPtr<FJsonObject>& Args)
{
	FString NameFilter;
	if (!Args->TryGetStringField(TEXT("name"), NameFilter) || NameFilter.IsEmpty())
		return {true, TEXT("{\"error\":\"name filter required\"}")};
	double NX = 0, NY = 0, NZ = 0, X = 0, Y = 0, Z = 0;
	Args->TryGetNumberField(TEXT("near_x"), NX);
	Args->TryGetNumberField(TEXT("near_y"), NY);
	Args->TryGetNumberField(TEXT("near_z"), NZ);
	double Radius = 500.0;
	Args->TryGetNumberField(TEXT("radius_m"), Radius);
	bool bDestroy = false;
	Args->TryGetBoolField(TEXT("destroy"), bDestroy);
	if (!Args->TryGetNumberField(TEXT("x"), X) || !Args->TryGetNumberField(TEXT("y"), Y) || !Args->TryGetNumberField(TEXT("z"), Z))
		return {true, TEXT("{\"error\":\"target x,y,z required\"}")};
	UWorld* World = GetGameWorld();
	if (!World) return {true, TEXT("{\"error\":\"no active game world\"}")};
	FString Result;
	RunOnGameThread([&World, NameFilter, NX, NY, NZ, Radius, X, Y, Z, bDestroy, &Result]()
	{
		const FVector Center(NX * 100.0, NY * 100.0, NZ * 100.0);
		const FString F = LowerUnicode(NameFilter);
		AFGBuildable* Best = nullptr;
		float BestD = TNumericLimits<float>::Max();
		for (TActorIterator<AFGBuildable> It(World); It; ++It)
		{
			AFGBuildable* B = *It;
			if (!B) continue;
			if (!LowerUnicode(B->GetClass()->GetName()).Contains(F)) continue;
			const float D = FVector::Dist(B->GetActorLocation(), Center);
			if (D < BestD && D < Radius * 100.0) { BestD = D; Best = B; }
		}
		if (!Best)
		{
			Result = TEXT("{\"error\":\"no matching buildable near\"}");
			return;
		}
		const FVector To(X * 100.0, Y * 100.0, Z * 100.0);
		FHitResult IgnoreHit;
		const bool bMoved = Best->K2_SetActorLocationAndRotation(To, Best->GetActorRotation(), false, IgnoreHit, true);
		if (bDestroy)
		{
			Best->K2_DestroyActor();
			Result = TEXT("{\"destroyed\":true}");
			return;
		}
		const FVector L = Best->GetActorLocation();
		Result = FString::Printf(TEXT("{\"moved\":%s,\"class\":\"%s\",\"from_d_m\":%.1f,\"at\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}}"),
			bMoved ? TEXT("true") : TEXT("false"), *Best->GetClass()->GetName(), BestD / 100.0, L.X / 100.0, L.Y / 100.0, L.Z / 100.0);
	});
	return Result.IsEmpty() ? FMcpToolResult{true, TEXT("{\"error\":\"game thread timeout\"}")} : FMcpToolResult{false, Result};
}

FMcpToolResult ToolSetPhase(const TSharedPtr<FJsonObject>& Args)
{
	double Index = 0;
	if (!Args->TryGetNumberField(TEXT("index"), Index))
		return {true, TEXT("{\"error\":\"index (game phase number) required\"}")};

	UWorld* World = GetGameWorld();
	if (!World) return {true, TEXT("{\"error\":\"no active game world\"}")};
	FString Result;
	RunOnGameThread([&World, Index, &Result]()
	{
		AFGGamePhaseManager* PM = AFGGamePhaseManager::Get(World);
		if (!PM)
		{
			Result = TEXT("{\"error\":\"phase manager unavailable\"}");
			return;
		}
		const bool bOk = PM->SetGamePhaseFromGamePhaseIndex((int32)Index);
		const int32 After = PM->GetGamePhaseIndexFromGamePhase(PM->GetCurrentGamePhase());
		Result = FString::Printf(TEXT("{\"ok\":%s,\"wanted\":%d,\"after\":%d}"),
			bOk ? TEXT("true") : TEXT("false"), (int32)Index, After);
	});
	return Result.IsEmpty() ? FMcpToolResult{true, TEXT("{\"error\":\"game thread timeout\"}")} : FMcpToolResult{false, Result};
}

FString FMcpTools::GetToolsListJson()
{
	static const TCHAR* ToolsJsonA = LR"json([
	{"name":"get_game_info","description":"Current map name and world time.","inputSchema":{"type":"object","properties":{}}},
	{"name":"get_player_state","description":"Player position (meters), yaw, health, velocity.","inputSchema":{"type":"object","properties":{}}},
	{"name":"list_buildables","description":"List built machines/structures with class and location. Args: max (int, default 100).","inputSchema":{"type":"object","properties":{"max":{"type":"integer"}}}},
	{"name":"get_buildables_summary","description":"Histogram of all built objects grouped by class name, sorted by count.","inputSchema":{"type":"object","properties":{}}},
	{"name":"find_buildables","description":"Find buildables by class-name substring and/or radius around a point (meters). Args: name (string), near_x/near_y/near_z (numbers), radius_m (default 100), limit (default 50).","inputSchema":{"type":"object","properties":{"name":{"type":"string"},"near_x":{"type":"number"},"near_y":{"type":"number"},"near_z":{"type":"number"},"radius_m":{"type":"number"},"limit":{"type":"integer"}}}},
	{"name":"get_player_inventory","description":"Player inventory: item display names and counts, total slots.","inputSchema":{"type":"object","properties":{}}},
	{"name":"list_storages","description":"Storage containers with contents (item names, counts). Args: limit (default 25), include_empty (bool), near_x/near_y/near_z + radius_m filter.","inputSchema":{"type":"object","properties":{"limit":{"type":"integer"},"include_empty":{"type":"boolean"},"near_x":{"type":"number"},"near_y":{"type":"number"},"near_z":{"type":"number"},"radius_m":{"type":"number"}}}},
	{"name":"set_recipe","description":"Set production recipe on nearest manufacturer to point by product name (localized substring). Args: x,y,z (meters), product, radius_m (default 10).","inputSchema":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"product":{"type":"string"},"radius_m":{"type":"number"}},"required":["x","y","z","product"]}},
	{"name":"get_ground_z","description":"Terrain/building surface Z (meters) at x,y via down-ward trace. Args: x,y (meters).","inputSchema":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"}},"required":["x","y"]}},
	{"name":"get_resource_nodes","description":"Resource nodes: resource name, purity (Impure/Normal/Pure), location in meters. Args: limit (default 60).","inputSchema":{"type":"object","properties":{"limit":{"type":"integer"}}}},
	{"name":"list_recipes","description":"Game crafting/build recipes (products + ingredients with display names; buildable recipes marked with hologram class). Args: item (substring filter on product names, e.g. 'conveyor'), limit (default 40).","inputSchema":{"type":"object","properties":{"item":{"type":"string"},"limit":{"type":"integer"}}}},
	{"name":"teleport_player","description":"Teleport the player to x,y,z in meters.","inputSchema":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}},"required":["x","y","z"]}},
	{"name":"lightweight_convert","description":"Convert lightweight instanced buildables (foundations!) into real actors within radius of a point.","inputSchema":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"radius_m":{"type":"number"}},"required":["x","y","z"]}},
	{"name":"lightweight_list","description":"List lightweight instanced buildables (foundations/walls) near x,y within radius_m: class, index, position.","inputSchema":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"radius_m":{"type":"number"}},"required":["x","y"]}},
	{"name":"lightweight_remove","description":"Remove lightweight instanced buildables near x,y within radius_m (cleanup).","inputSchema":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"radius_m":{"type":"number"}},"required":["x","y"]}},
	{"name":"spawn_buildable","description":"Spawn a buildable class directly (for special classes with no build recipe, e.g. Build_Hub_C). Args: query (exact class name preferred), x,y,z (meters), yaw (deg).","inputSchema":{"type":"object","properties":{"query":{"type":"string"},"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"yaw":{"type":"number"}},"required":["query"]}},
	{"name":"move_buildable","description":"Move the nearest buildable whose class contains 'name' (from near_x/y/z within radius_m) to x,y,z. Useful for relocating stasis-parked buildables. Args: name, near_x,near_y,near_z, radius_m, x,y,z.","inputSchema":{"type":"object","properties":{"name":{"type":"string"},"near_x":{"type":"number"},"near_y":{"type":"number"},"near_z":{"type":"number"},"radius_m":{"type":"number"},"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}},"required":["name","x","y","z"]}},
)json";
	static const TCHAR* ToolsJsonA2 = LR"json(
	{"name":"set_phase","description":"Set the game phase (legit advanced game rule, unlocks research trees up to that phase). Args: index (0-based phase number).","inputSchema":{"type":"object","properties":{"index":{"type":"integer"}},"required":["index"]}},
	{"name":"give_item","description":"Give items to the player inventory (or nearest storage at storage_x/y/z). Args: item (display name like 'Стальной слиток' or class), count (default 1, max 10000), optional storage_x/storage_y/storage_z.","inputSchema":{"type":"object","properties":{"item":{"type":"string"},"count":{"type":"integer"},"storage_x":{"type":"number"},"storage_y":{"type":"number"},"storage_z":{"type":"number"}},"required":["item"]}},
	{"name":"build","description":"Build through the game's hologram pipeline exactly like a player: recipe match -> hologram placement/snapping/ground adjust -> game validates (clearance, floor, materials) -> Construct. Consumes cost from player inventory. Args: query (product display name or class substring, e.g. 'Smelter', 'Фундамент'), x,y,z (meters), yaw (deg), grid (number: snap x,y to world-grid cell centers, use 8 for foundations), flat_z (number meters: force the whole piece onto one absolute Z plane, level floors over uneven terrain), dry_run (bool: run full placement+validation, report valid/reasons WITHOUT constructing).","inputSchema":{"type":"object","properties":{"query":{"type":"string"},"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"yaw":{"type":"number"},"grid":{"type":"number"},"flat_z":{"type":"number"},"dry_run":{"type":"boolean"}},"required":["query","x","y","z"]}},
	{"name":"build_belt","description":"Lay a conveyor like a player: belt hologram snaps to OUTPUT connector near from_* and INPUT connector near to_*, game routes the spline (bends, inclines, support poles) and connects both ends itself. Args: from_x,from_y,from_z,to_x,to_y,to_z (meters), radius_m (default 6), query (belt product name, default 'Conveyor Belt Mk1'), mode ('straight' = game orthogonal build mode, only 90-degree corners; 'curve' = free spline mode; both native in-game modes), via ([{x,y,z}] meters: intermediate anchors clicked before finishing, use with curve for multi-bend runs), max_steps (default 10), dry_run (bool)).","inputSchema":{"type":"object","properties":{"from_x":{"type":"number"},"from_y":{"type":"number"},"from_z":{"type":"number"},"to_x":{"type":"number"},"to_y":{"type":"number"},"to_z":{"type":"number"},"radius_m":{"type":"number"},"query":{"type":"string"},"mode":{"type":"string","enum":["straight","curve"]},"via":{"type":"array","items":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}}}},"max_steps":{"type":"integer"},"dry_run":{"type":"boolean"}},"required":["from_x","from_y","from_z","to_x","to_y","to_z"]}},
	{"name":"build_many","description":"Batch build in one call: items[] where each item has the same fields as build (query,x,y,z,yaw,grid,flat_z,dry_run). Runs sequentially through the same hologram pipeline; returns per-item results plus built/failed counts.","inputSchema":{"type":"object","properties":{"items":{"type":"array","items":{"type":"object","properties":{"query":{"type":"string"},"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"yaw":{"type":"number"},"grid":{"type":"number"},"flat_z":{"type":"number"},"dry_run":{"type":"boolean"}},"required":["query","x","y","z"]}}},"required":["items"]}},
)json";
	static const TCHAR* ToolsJsonB = LR"json(
	{"name":"wire","description":"Run a power line like a player: wire hologram snaps between the power connector near x1,y1,z1 and the one near x2,y2,z2 (machine<->pole or pole<->pole), then verifies both ends share one power circuit. Args: x1,y1,z1,x2,y2,z2 (meters), r1,r2 search radius m.","inputSchema":{"type":"object","properties":{"x1":{"type":"number"},"y1":{"type":"number"},"z1":{"type":"number"},"x2":{"type":"number"},"y2":{"type":"number"},"z2":{"type":"number"},"r1":{"type":"number"},"r2":{"type":"number"}},"required":["x1","y1","z1","x2","y2","z2"]}},
	{"name":"get_machine_status","description":"Factories near point: class, location, current recipe products, connections (in/out, connected, item on output). Args: near_x,near_y,near_z,radius_m (default 50), class substring filter, limit (default 15).","inputSchema":{"type":"object","properties":{"near_x":{"type":"number"},"near_y":{"type":"number"},"near_z":{"type":"number"},"radius_m":{"type":"number"},"class":{"type":"string"},"limit":{"type":"integer"}},"required":["near_x","near_y","near_z"]}},
	{"name":"power_check","description":"Power diagnostics near point: generators (canStart, capacity MW) and consumers actual vs target consumption MW (0 actual = unpowered or idle). Args: near_x,near_y,radius_m.","inputSchema":{"type":"object","properties":{"near_x":{"type":"number"},"near_y":{"type":"number"},"radius_m":{"type":"number"}},"required":["near_x","near_y"]}},
	{"name":"dismantle","description":"Dismantle nearest buildable to x,y,z within radius_m (optional name substring filter). Args: x,y,z,radius_m,name,apply (default true; false = preview target only).","inputSchema":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"radius_m":{"type":"number"},"name":{"type":"string"},"apply":{"type":"boolean"}},"required":["x","y","z"]}}
])json";
	return FString(ToolsJsonA) + FString(ToolsJsonA2) + FString(ToolsJsonB);
}

FMcpToolResult FMcpTools::Call(const FString& ToolName, const FString& ParamsJson)
{
	const TSharedPtr<FJsonObject> Args = ParseArgs(ParamsJson);

	if (ToolName == TEXT("get_game_info"))
	{
		FString Result;
		if (!RunOnGameThread([&Result]()
			{
				UWorld* World = GetGameWorld();
				Result = World
					? FString::Printf(TEXT("{\"mapName\":\"%s\",\"worldTimeSeconds\":%.1f}"), *World->GetMapName(), World->GetTimeSeconds())
					: FString(TEXT("{\"error\":\"no active game world\"}"));
			}))
		{
			return {true, TEXT("{\"error\":\"game thread timeout\"}")};
		}
		return {false, Result};
	}
	if (ToolName == TEXT("get_player_state"))
	{
		return ToolGetPlayerState();
	}
	if (ToolName == TEXT("list_buildables"))
	{
		return ToolListBuildables(Args);
	}
	if (ToolName == TEXT("get_buildables_summary"))
	{
		return ToolBuildablesSummary();
	}
	if (ToolName == TEXT("find_buildables"))
	{
		return ToolFindBuildables(Args);
	}
	if (ToolName == TEXT("get_player_inventory"))
	{
		return ToolPlayerInventory();
	}
	if (ToolName == TEXT("list_storages"))
	{
		return ToolListStorages(Args);
	}
	if (ToolName == TEXT("get_resource_nodes"))
	{
		return ToolResourceNodes(Args);
	}
	if (ToolName == TEXT("list_recipes"))
	{
		return ToolListRecipes(Args);
	}
	if (ToolName == TEXT("set_recipe"))
	{
		return ToolSetRecipe(Args);
	}
	if (ToolName == TEXT("get_ground_z"))
	{
		return ToolGroundZ(Args);
	}
	if (ToolName == TEXT("build_many"))
		return ToolBuildMany(Args);
	if (ToolName == TEXT("build_belt"))
	{
		return ToolBuildBelt(Args);
	}
	if (ToolName == TEXT("power_check"))
		return ToolPowerCheck(Args);
	if (ToolName == TEXT("wire"))
		return ToolWire(Args);
	if (ToolName == TEXT("get_machine_status"))
	{
		return ToolMachineStatus(Args);
	}
	if (ToolName == TEXT("give_item"))
	{
		return ToolGiveItem(Args);
	}
	if (ToolName == TEXT("build"))
	{
		return ToolBuild(Args);
	}
	if (ToolName == TEXT("dismantle"))
	{
		return ToolDismantle(Args);
	}
	if (ToolName == TEXT("teleport_player"))
	{
		return ToolTeleportPlayer(Args);
	}
	if (ToolName == TEXT("lightweight_convert"))
	{
		return ToolLightweightConvert(Args);
	}
	if (ToolName == TEXT("lightweight_list"))
	{
		return ToolLightweightList(Args);
	}
	if (ToolName == TEXT("lightweight_remove"))
	{
		return ToolLightweightRemove(Args);
	}
	if (ToolName == TEXT("construct_test"))
	{
		return ToolConstructTest(Args);
	}
	if (ToolName == TEXT("spawn_buildable"))
	{
		return ToolSpawnBuildable(Args);
	}
	if (ToolName == TEXT("move_buildable"))
	{
		return ToolMoveBuildable(Args);
	}
	if (ToolName == TEXT("set_phase"))
	{
		return ToolSetPhase(Args);
	}

	FMcpToolResult Unknown;
	Unknown.bIsError = true;
	Unknown.Text = FString::Printf(TEXT("{\"error\":\"unknown tool: %s\"}"), *ToolName);
	return Unknown;
}
