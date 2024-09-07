////////////////////////////////////////////////////////////////////////
//
// This file is part of sc4-remove-phantom-budget-items, a DLL Plugin
// for SimCity 4 that removes phantom items from the budget department
// lists.
//
// Copyright (c) 2024 Nicholas Hayes
//
// This file is licensed under terms of the MIT License.
// See LICENSE.txt for more information.
//
////////////////////////////////////////////////////////////////////////

#include "cIGZCheatCodeManager.h"
#include "cIGZCOM.h"
#include "cIGZFrameWork.h"
#include "cIGZMessage2Standard.h"
#include "cIGZMessageServer2.h"
#include "cISC4App.h"
#include "cISC4BudgetSimulator.h"
#include "cISC4BuildingOccupant.h"
#include "cISC4City.h"
#include "cISC4DepartmentBudget.h"
#include "cISC4LineItem.h"
#include "cISC4Occupant.h"
#include "cISCPropertyHolder.h"
#include "cRZAutoRefCount.h"
#include "cRZBaseString.h"
#include "cRZMessage2COMDirector.h"
#include "DebugUtil.h"
#include "GZServPtrs.h"
#include "Logger.h"
#include "SC4List.h"
#include "SC4NotificationDialog.h"
#include "StringViewUtil.h"
#include "version.h"

#include <array>
#include <string>
#include <unordered_set>
#include <vector>

#include <Windows.h>
#include "wil/resource.h"
#include "wil/result.h"
#include "wil/win32_helpers.h"

using namespace std::string_view_literals;

static constexpr uint32_t kSC4MessagePostCityInit = 0x26D31EC1;
static constexpr uint32_t kSC4MessagePostCityShutdown = 0x26D31EC3;
static constexpr uint32_t kMessageCheatIssued = 0x230E27AC;

static constexpr std::array<uint32_t, 2> RequiredNotifications =
{
	kSC4MessagePostCityInit,
	kSC4MessagePostCityShutdown
};

static constexpr uint32_t kRemovePhantomBudgetItemsDllDirector = 0x6A702330;

static constexpr uint32_t kRemovePhantomBudgetItemsCheatID = 0x7630FE7F;
static const char* kRemovePhantomBudgetItemsCheatString = "RemovePhantomBudgetItems";

static constexpr std::string_view PluginLogFileName = "SC4RemovePhantomBudgetItems.log"sv;

namespace
{
	std::filesystem::path GetDllFolderPath()
	{
		wil::unique_cotaskmem_string modulePath = wil::GetModuleFileNameW(wil::GetModuleInstanceHandle());

		std::filesystem::path temp(modulePath.get());

		return temp.parent_path();
	}

	void ShowNotificationDialog(const cIGZString& message)
	{
		SC4NotificationDialog::ShowDialog(message, cRZBaseString(kRemovePhantomBudgetItemsCheatString));
	}

	bool ParseBudgetCategoryName(const std::string_view& input, std::vector<std::pair<uint32_t, uint32_t>>& outDepartmentAndPurpose)
	{
		outDepartmentAndPurpose.clear();

		if (StringViewUtil::EqualsIgnoreCase(input, "Fire"sv))
		{
			outDepartmentAndPurpose.push_back(std::pair(0x28F55A9F, 0xEA567BC3));
		}
		else if (StringViewUtil::EqualsIgnoreCase(input, "Police"sv))
		{
			outDepartmentAndPurpose.push_back(std::pair(0xA2963983, 0x0A567BAA));
		}
		else if (StringViewUtil::EqualsIgnoreCase(input, "Jail"sv))
		{
			outDepartmentAndPurpose.push_back(std::pair(0xA2963984, 0xEA56768A));
		}
		else if (StringViewUtil::EqualsIgnoreCase(input, "Power"sv))
		{
			outDepartmentAndPurpose.push_back(std::pair(0x8910BC8A, 0xCA58E540));
		}
		else if (StringViewUtil::EqualsIgnoreCase(input, "Education"sv))
		{
			outDepartmentAndPurpose.push_back(std::pair(0x09188F4C, 0xEA5654B6)); // Education Staff
			outDepartmentAndPurpose.push_back(std::pair(0x4A538CC6, 0x4A5654BA)); // Education Coverage
		}
		else if (StringViewUtil::EqualsIgnoreCase(input, "Health"sv))
		{
			outDepartmentAndPurpose.push_back(std::pair(0x09188F42, 0xCA565486)); // Health Staff
			outDepartmentAndPurpose.push_back(std::pair(0xAA538CB3, 0xEA56549E)); // Health Coverage
		}

		return !outDepartmentAndPurpose.empty();
	}

	bool GetBudgetItemsToRemove(
		cISC4DepartmentBudget* pDepartmentBudget,
		uint32_t purposeID,
		bool clearAllItems,
		std::vector<cRZAutoRefCount<cISC4BuildingOccupant>>& orphanedBuildingOccupants)
	{
		constexpr uint32_t kExemplarType = 0x00000010;

		orphanedBuildingOccupants.clear();

		SC4List<cISCPropertyHolder> locallyFundedItems;

		if (pDepartmentBudget->GetLocallyFundedItemsByPurpose(purposeID, locallyFundedItems))
		{
			for (cISCPropertyHolder& propertyHolder : locallyFundedItems)
			{
				// If the property holder doesn't have an exemplar type property that
				// indicates the item has been deleted from the plugin folder.

				if (clearAllItems || !propertyHolder.HasProperty(kExemplarType))
				{
					cRZAutoRefCount<cISC4BuildingOccupant> buildingOccupant;

					if (propertyHolder.QueryInterface(GZIID_cISC4BuildingOccupant, buildingOccupant.AsPPVoid()))
					{
						orphanedBuildingOccupants.push_back(std::move(buildingOccupant));
					}
				}
			}
		}

		return !orphanedBuildingOccupants.empty();
	}
}

class RemovePhantomBudgetItemsDllDirector final : public cRZMessage2COMDirector
{
public:

	RemovePhantomBudgetItemsDllDirector() : pBudgetSim(nullptr)
	{
		std::filesystem::path dllFolderPath = GetDllFolderPath();

		std::filesystem::path logFilePath = dllFolderPath;
		logFilePath /= PluginLogFileName;

		Logger& logger = Logger::GetInstance();
		logger.Init(logFilePath, LogLevel::Error);
		logger.WriteLogFileHeader("SC4RemovePhantomBudgetItems v" PLUGIN_VERSION_STR);
	}

	uint32_t GetDirectorID() const
	{
		return kRemovePhantomBudgetItemsDllDirector;
	}

	bool OnStart(cIGZCOM* pCOM)
	{
		cIGZFrameWork* const pFramework = pCOM->FrameWork();

		if (pFramework->GetState() < cIGZFrameWork::kStatePreAppInit)
		{
			pFramework->AddHook(this);
		}
		else
		{
			PreAppInit();
		}

		return true;
	}

private:
	void PostCityInit(cIGZMessage2Standard* pStandardMsg)
	{
		cISC4City* pCity = static_cast<cISC4City*>(pStandardMsg->GetVoid1());

		if (pCity)
		{
			pBudgetSim = pCity->GetBudgetSimulator();

			cISC4AppPtr pSC4App;

			if (pSC4App)
			{
				cIGZCheatCodeManager* pCheatCodeManager = pSC4App->GetCheatCodeManager();

				if (pCheatCodeManager)
				{
					pCheatCodeManager->AddNotification2(this, 0);
					pCheatCodeManager->RegisterCheatCode(
						kRemovePhantomBudgetItemsCheatID,
						cRZBaseString(kRemovePhantomBudgetItemsCheatString));
				}
			}
		}
	}

	void PostCityShutdown()
	{
		pBudgetSim = nullptr;

		cISC4AppPtr pSC4App;

		if (pSC4App)
		{
			cIGZCheatCodeManager* pCheatCodeManager = pSC4App->GetCheatCodeManager();

			if (pCheatCodeManager)
			{
				pCheatCodeManager->RemoveNotification2(this, 0);
				pCheatCodeManager->UnregisterCheatCode(kRemovePhantomBudgetItemsCheatID);
			}
		}
	}

	void ProcessCheatCodes(cIGZMessage2Standard* pStandardMsg)
	{
		const uint32_t cheatID = pStandardMsg->GetData1();

		if (cheatID == kRemovePhantomBudgetItemsCheatID)
		{
			const cIGZString* pCheatString = static_cast<const cIGZString*>(pStandardMsg->GetVoid2());

			std::vector<std::string_view> arguments;
			StringViewUtil::Split(pCheatString->ToChar(), ' ', arguments);

			if (arguments.size() == 2 || arguments.size() == 3)
			{
				const std::string_view category = arguments[1];
				const bool clearAllItems = arguments.size() == 3 && StringViewUtil::EqualsIgnoreCase(arguments[2], "all"sv);

				std::vector<std::pair<uint32_t, uint32_t>> departmentAndPurpose;
				departmentAndPurpose.reserve(2);

				if (ParseBudgetCategoryName(category, departmentAndPurpose))
				{
					if (pBudgetSim)
					{
						std::unordered_set<uint32_t> orphanedBuildings;

						for (const auto& item : departmentAndPurpose)
						{
							cISC4DepartmentBudget* pDepartmentBudget = pBudgetSim->GetDepartmentBudget(item.first);

							if (pDepartmentBudget)
							{
								std::vector<cRZAutoRefCount<cISC4BuildingOccupant>> buldingOccupants;

								if (GetBudgetItemsToRemove(
									pDepartmentBudget,
									item.second,
									clearAllItems,
									buldingOccupants))
								{
									for (cISC4BuildingOccupant* pBuildingOccupant : buldingOccupants)
									{
										// Remove the building from the game's locally funded item list.
										pDepartmentBudget->RemoveLocallyFundedObject(
											pBuildingOccupant->AsOccupant()->AsPropertyHolder(),
											item.second);

										const uint32_t buildingType = pBuildingOccupant->GetBuildingType();

										// Some buildings may also have an associated line item.
										pDepartmentBudget->RemoveLineItem(buildingType);

										if (!clearAllItems)
										{
											// The orphaned buildings are counted by building type because the Education and
											// Health categories can have the same building in two different departments.
											orphanedBuildings.emplace(buildingType);
										}
									}
								}
							}
						}

						cRZBaseString message;

						if (clearAllItems)
						{
							message.Append("Cleared the ").Append(category).Append(" budget category.");
						}
						else
						{
							if (orphanedBuildings.empty())
							{
								message.FromChar("No orphaned budget items found.");
							}
							else
							{
								message.Sprintf("Removed %u orphaned budget item(s).", orphanedBuildings.size());
							}
						}

						ShowNotificationDialog(message);
					}
				}
				else
				{
					cRZBaseString message("Unknown Budget Category: ");
					message.Append(category);

					ShowNotificationDialog(message);
				}
			}
			else
			{
				cRZBaseString message(
					"Usage: RemovePhantomBudgetItems <category> [all]\n"
					"<category>: One of Fire, Police, Jail, Power, Education or Health.\n"
					"[all] A non-default option that clears the entire budget category, instead of only plugin(s) that no longer exist.");

				ShowNotificationDialog(message);
			}
		}
	}

	bool DoMessage(cIGZMessage2* pMsg)
	{
		cIGZMessage2Standard* pStandardMsg = static_cast<cIGZMessage2Standard*>(pMsg);

		switch (pStandardMsg->GetType())
		{
		case kMessageCheatIssued:
			ProcessCheatCodes(pStandardMsg);
			break;
		case kSC4MessagePostCityInit:
			PostCityInit(pStandardMsg);
			break;
		case kSC4MessagePostCityShutdown:
			PostCityShutdown();
			break;
		}

		return true;
	}


	bool PostAppInit()
	{
		Logger& logger = Logger::GetInstance();

		cIGZMessageServer2Ptr pMS2;

		if (pMS2)
		{
			for (uint32_t messageID : RequiredNotifications)
			{
				if (!pMS2->AddNotification(this, messageID))
				{
					logger.WriteLine(LogLevel::Error, "Failed to subscribe to the required notifications.");
					return false;
				}
			}
		}
		else
		{
			logger.WriteLine(LogLevel::Error, "Failed to subscribe to the required notifications.");
			return false;
		}

		return true;
	}

	cISC4BudgetSimulator* pBudgetSim;
};

cRZCOMDllDirector* RZGetCOMDllDirector() {
	static RemovePhantomBudgetItemsDllDirector sDirector;
	return &sDirector;
}