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

#pragma once
#include <cstdint>

class SC4VersionDetection
{
public:

	static SC4VersionDetection& GetInstance();

	uint16_t GetGameVersion() const noexcept;

private:

	SC4VersionDetection();

	uint16_t gameVersion;
};

