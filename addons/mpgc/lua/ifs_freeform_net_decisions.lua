-- Preserve nil return slots around the few native callbacks that require an actor view.
local function net_decision_results(...) return arg end

local function net_decision_noop() end

local net_actor_fields = {
	"playerTeam", "playerSide", "otherSide", "joystick", "joystick_other",
	"planetSelected", "planetNext", "lastSelected", "lastFleet", "curScreen"
}

-- Native serializers and AI read main's actor fields; the registered screens always retain the local view.
function ifs_freeform_net_fnWithActor(callback, this, ...)
	local main, decision, view = ifs_freeform_main, ifs_freeform_net_campaign.decision, {}
	for _, field in ipairs(net_actor_fields) do
		view[field] = main[field]
	end

	main:SetActiveTeam(decision.playerTeam)
	main.planetSelected, main.planetNext = decision.planetSelected, decision.planetNext
	main.lastSelected, main.lastFleet, main.curScreen = decision.lastSelected, decision.lastFleet, decision.curScreen
	local result = net_decision_results(pcall(callback, this, unpack(arg, 1, arg.n)))

	for _, field in ipairs(net_actor_fields) do
		main[field] = view[field]
	end
	main:SetActiveTeam(view.playerTeam)

	if not result[1] then
		error(result[2])
	end

	return unpack(result, 2, result.n)
end

-- Retain native saved-step meanings instead of reconstructing a screen stack.
function ifs_freeform_net_fnSavedStep(name)
	local decision = ifs_freeform_net_campaign.decision
	if name == "ifs_freeform_main" then
		return true
	end
	if name == decision.screen then
		return true
	end
	if name == "ifs_freeform_result" then
		return decision.curScreen == "summary_result"
	end
	if name == "ifs_freeform_fleet" then
		return decision.curScreen == "battle_back" or decision.curScreen == "summary_fleet"
	end
	return false
end

-- Choose the native encounter's deciding human without changing its attacker.
function ifs_freeform_net_fnDecisionFaction(decision)
	local team, commanders = decision.playerTeam, ifs_freeform_net_campaign.state.commanders
	if decision.screen == "ifs_freeform_battle_mode" and commanders[3 - team] >= 0 then
		return 3 - team
	end
	if (decision.screen == "ifs_freeform_battle" or decision.screen == "ifs_freeform_result")
		and commanders[team] < 0 and commanders[3 - team] >= 0 then
		return 3 - team
	end
	return team
end

-- Preserve the native turn/victory branch with explicit actor and remembered selections.
function ifs_freeform_net_fnNextTurn()
	local decision, main = ifs_freeform_net_campaign.decision, ifs_freeform_main
	if main.CheckVictory then
		main.teamVictory = main:CheckVictory()
	end
	if main.teamVictory then
		ifs_freeform_net_fnAdvanceDecision("ifs_freeform_end")
		return
	end

	main.turnNumber = main.turnNumber + 1
	decision.lastSelected[decision.playerTeam] = decision.planetNext
	decision.playerTeam = 3 - decision.playerTeam
	decision.planetSelected = decision.lastSelected[decision.playerTeam]
	decision.planetNext = decision.planetSelected
	decision.move, decision.pending, decision.presentation = nil, nil, nil
	main.launchMission, main.activeBonus = nil, {}
	ifs_freeform_net_fnAdvanceDecision(ifs_freeform_net_campaign.state.commanders[decision.playerTeam] >= 0
		and "ifs_freeform_fleet" or "ifs_freeform_ai")
end

-- Build only native held-card metadata; controls belong exclusively to the local faction's screen.
function ifs_freeform_net_fnBonusItems(team)
	local items, main = {}, ifs_freeform_main
	for slot, index in ipairs(ifs_purchase_tech_using[team]) do
		if index > 0 then
			local tech, weight = ifs_purchase_tech_table[index], 0
			for _, hint in ipairs(tech.hints) do
				if string.find(main.launchMission, hint[1]) then
					weight = hint[2]
					break
				end
			end
			table.insert(items, {
				slot = slot,
				index = index,
				bonus = tech.bonus,
				weight = weight
			})
		end
	end

	return items
end

-- Consume a held card once and preserve native three-second reveal or half-second Skip.
function ifs_freeform_net_fnAcceptBonus(team, slot)
	local bonus, main = ifs_freeform_net_campaign.decision.bonus, ifs_freeform_main
	local choice = bonus and bonus.choices[team]
	if not choice or choice.accepted then
		return false
	end

	local selected
	if slot ~= 0 then
		for _, item in ipairs(choice.items) do
			if item.slot == slot and item.weight > 0
				and ifs_purchase_tech_using[team][slot] == item.index then
				selected = item
			end
		end
		if not selected then
			return false, "That bonus cannot be used for this battle."
		end
	end

	choice.accepted, choice.selected, choice.remaining = true, selected, selected and 3 or 0.5
	if selected then
		main.activeBonus[team] = selected.bonus
		ActivateBonus(team, "team_bonus_" .. selected.bonus)
		ifs_purchase_tech_using[team][selected.slot] = 0
	end
	return true
end

-- Preserve native AI's card weights and its extra weight of two for choosing no bonus.
function ifs_freeform_net_fnChooseAIBonus(team)
	local choice = ifs_freeform_net_campaign.decision.bonus.choices[team]
	local weight = 2
	for _, item in ipairs(choice.items) do
		weight = weight + item.weight
	end

	local random = math.random() * weight
	local slot = 0
	for _, item in ipairs(choice.items) do
		random = random - item.weight
		if random <= 0 then
			slot = item.slot
			break
		end
	end

	ifs_freeform_net_fnAcceptBonus(team, slot)
end

-- Report unfinished choices and their native reveals in the shared bonus deadline.
function ifs_freeform_net_fnBonusMask()
	local bonus = ifs_freeform_net_campaign.decision and ifs_freeform_net_campaign.decision.bonus
	if not bonus or bonus.launched then
		return 0
	end

	local mask = 0
	for team = 1, 2 do
		if not bonus.choices[team].done then
			mask = mask + team
		end
	end
	return mask
end

-- Prepare the exact native campaign/setup pair once, with the original attacker restored.
function ifs_freeform_net_fnFinishBonuses()
	local decision, main = ifs_freeform_net_campaign.decision, ifs_freeform_main
	local bonus = decision.bonus
	if bonus.launched or ifs_freeform_net_fnBonusMask() ~= 0 then
		return
	end

	bonus.launched = true
	decision.playerTeam, decision.decisionFaction = bonus.attacker, bonus.attacker
	ScriptCB_SetSplitscreen(main.wasSplit)
	main:SaveState()
	ifs_freeform_net_fnWithActor(main.SaveMissionSetup, main)
	ScriptCB_SetMetagameSummary({ map = main.launchMission })
	local prepared, message = ScriptCB_PrepareMetagameBattle()
	if not prepared then
		ifs_freeform_net_fnSessionError(message)
	end
end

-- Start simultaneous choices; ordinary saves may already contain an accepted first-side bonus.
function ifs_freeform_net_fnStartBonuses(resumed)
	local decision, main = ifs_freeform_net_campaign.decision, ifs_freeform_main
	local attacker = decision.playerTeam
	local defending = resumed and decision.curScreen == "battle_card_2"
	if defending then
		attacker = 3 - attacker
	end

	local bonus = {
		attacker = attacker,
		choices = {}
	}
	decision.bonus, decision.playerTeam, decision.decisionFaction = bonus, attacker, attacker
	if not resumed then
		main.activeBonus = {}
	end

	for team = 1, 2 do
		local choice = { items = ifs_freeform_net_fnBonusItems(team) }
		bonus.choices[team] = choice

		-- The native defender step records a completed attacker choice, including Skip.
		if resumed and (main.activeBonus[team] or defending and team == attacker) then
			choice.accepted, choice.done = true, true
		elseif table.getn(choice.items) == 0 then
			choice.accepted, choice.done = true, true
		elseif ifs_freeform_net_campaign.state.commanders[team] < 0 then
			ifs_freeform_net_fnChooseAIBonus(team)
		end
	end

	decision.curScreen = "battle_card_1"
	ifs_freeform_net_fnFinishBonuses()
end

-- Enter the next decision using native saved-step semantics and the original AI algorithm.
function ifs_freeform_net_fnAdvanceDecision(name, savedStep)
	local decision, main = ifs_freeform_net_campaign.decision, ifs_freeform_main
	local previous = decision.screen
	decision.screen, decision.bonus = name, nil
	decision.curScreen = savedStep

	if name == "ifs_freeform_battle" then
		decision.curScreen = savedStep or (decision.move and "battle_back" or "battle_noback")
	elseif name == "ifs_freeform_summary" then
		decision.curScreen = savedStep or (previous == "ifs_freeform_result" and "summary_result" or "summary_fleet")
	elseif name == "ifs_freeform_result" then
		decision.curScreen = "result"
	elseif name == "ifs_freeform_battle_mode" then
		decision.curScreen = "battle_mode"
	elseif name == "ifs_freeform_battle_card" then
		decision.curScreen = "battle_card_1"
	end
	decision.decisionFaction = ifs_freeform_net_fnDecisionFaction(decision)

	if name == "ifs_freeform_ai" then
		-- Algorithm methods and weights remain native; this record owns only the running phase's data.
		decision.ai = setmetatable({}, { __index = ifs_freeform_ai })
		ifs_freeform_net_fnRunAI(ifs_freeform_ai.mpgc_enter, decision.ai, true)
	elseif name == "ifs_freeform_battle_mode" then
		local target = decision.planetNext
		decision.modes = main.planetFleet[target] == 0 and main.spaceMission or main.planetMission[target]
		local count, first = 0, nil
		for mode, missions in pairs(decision.modes) do
			count, first = count + 1, first or mode
		end

		for _, desc in ipairs(ifs_freeform_battle_vbutton_layout.buttonlist) do
			if decision.modes[desc.tag] then
				first = desc.tag
				break
			end
		end

		if count <= 1 or ifs_freeform_net_campaign.state.commanders[decision.decisionFaction] < 0 then
			main:SetLaunchMission(decision.modes[first])
			ifs_freeform_net_fnAdvanceDecision("ifs_freeform_battle_card")
		end
	elseif name == "ifs_freeform_battle_card" then
		ifs_freeform_net_fnStartBonuses(false)
	elseif name == "ifs_freeform_summary"
		and ifs_freeform_net_campaign.state.commanders[decision.playerTeam] < 0 then
		ifs_freeform_net_fnNextTurn()
	end
end

-- Run only the native AI phases under scoped camera/drawing/navigation adapters.
function ifs_freeform_net_fnRunAI(callback, this, ...)
	local decision, main = ifs_freeform_net_campaign.decision, ifs_freeform_main
	local push, camera, zoom, offset, snap = ScriptCB_PushScreen, MoveCameraToEntity,
		SetMapCameraZoom, SetMapCameraOffset, SnapMapCamera
	local particle, beam, common = DrawParticleAt, DrawBeamBetween, this.UpdateCommon
	local selected = main.SelectPlanet

	ScriptCB_PushScreen = function(name)
		decision.planetSelected, decision.planetNext = main.planetSelected, main.planetNext
		ifs_freeform_net_fnAdvanceDecision(name)
	end
	MoveCameraToEntity, SetMapCameraZoom, SetMapCameraOffset, SnapMapCamera = net_decision_noop,
		net_decision_noop, net_decision_noop, net_decision_noop
	DrawParticleAt, DrawBeamBetween, this.UpdateCommon = net_decision_noop, net_decision_noop, net_decision_noop
	main.SelectPlanet = function(main, info, planet)
		main.planetSelected, main.planetNext = planet, planet
	end

	ifs_freeform_net_campaign.executing = true
	local result = net_decision_results(pcall(ifs_freeform_net_fnWithActor, function(this, ...)
		local commanders = ifs_freeform_net_campaign.state.commanders
		main.joystick = commanders[decision.playerTeam] >= 0 and main.startController or nil
		main.joystick_other = commanders[3 - decision.playerTeam] >= 0 and main.startController or nil
		local returned = net_decision_results(callback(this, unpack(arg, 1, arg.n)))

		-- Native AI selects and moves through main; retain only its actor selections for the next phase.
		if decision.screen == "ifs_freeform_ai" then
			decision.planetSelected, decision.planetNext = main.planetSelected, main.planetNext
		end

		return unpack(returned, 1, returned.n)
	end, this, unpack(arg, 1, arg.n)))

	ifs_freeform_net_campaign.executing = nil
	ScriptCB_PushScreen, MoveCameraToEntity, SetMapCameraZoom, SetMapCameraOffset, SnapMapCamera =
		push, camera, zoom, offset, snap
	DrawParticleAt, DrawBeamBetween, this.UpdateCommon, main.SelectPlanet = particle, beam, common, selected

	if not result[1] then
		error(result[2])
	end

	return unpack(result, 2, result.n)
end

-- Serialize through native SaveState while temporarily supplying its actor, undo and saved-step inputs.
function ifs_freeform_net_fnSaveDecision(callback, this, ...)
	if ifs_freeform_net_campaign.diskPending then
		return
	end

	local decision, main, fleet = ifs_freeform_net_campaign.decision, ifs_freeform_main, ifs_freeform_fleet
	if not decision then
		return callback(this, unpack(arg, 1, arg.n))
	end

	local stacked, defending = ScriptCB_IsScreenInStack, ifs_freeform_battle_card.defending
	local turn, source, target = fleet.turnNumber, fleet.planetStart, fleet.planetNext
	local resources, using, active = main.teamResources, ifs_purchase_tech_using, main.activeBonus

	ScriptCB_IsScreenInStack = ifs_freeform_net_fnSavedStep
	ifs_freeform_battle_card.defending = decision.curScreen == "battle_card_2"
	local move = decision.move
	fleet.turnNumber, fleet.planetStart, fleet.planetNext =
		move and move.turn, move and move.source, move and move.target
	if ifs_freeform_net_campaign.diskNormalize and decision.pending then
		main.teamResources = { resources[1], resources[2] }
		local pending = decision.pending
		main.teamResources[pending.faction] = main.teamResources[pending.faction] + pending.paid
	end

	local actor = decision.playerTeam
	if ifs_freeform_net_campaign.diskNormalize and decision.bonus and not decision.bonus.launched then
		-- Disk saves resume sequential native selection; return unlaunched cards only in this serialized view.
		main.activeBonus, ifs_purchase_tech_using = {}, { {}, {} }
		for team = 1, 2 do
			for slot, index in ipairs(using[team]) do
				ifs_purchase_tech_using[team][slot] = index
			end
			local item = decision.bonus.choices[team].selected
			if item then
				ifs_purchase_tech_using[team][item.slot] = item.index
			end
		end
		decision.playerTeam = decision.bonus.attacker
		ifs_freeform_battle_card.defending = nil
	end

	local result = net_decision_results(pcall(ifs_freeform_net_fnWithActor,
		callback, this, unpack(arg, 1, arg.n)))

	decision.playerTeam = actor
	main.teamResources, ifs_purchase_tech_using, main.activeBonus = resources, using, active
	ScriptCB_IsScreenInStack, ifs_freeform_battle_card.defending = stacked, defending
	fleet.turnNumber, fleet.planetStart, fleet.planetNext = turn, source, target

	if not result[1] then
		error(result[2])
	end

	return unpack(result, 2, result.n)
end

-- Publish native bytes and the small decision fields omitted from the original disk format.
function ifs_freeform_net_fnPublishDecision()
	local decision, main = ifs_freeform_net_campaign.decision, ifs_freeform_main
	main:SaveState()
	ifs_freeform_net_fnRememberCampaign()

	local phase, source, remaining = ifs_freeform_net_fnPresentationState()
	local strategic = decision.screen == "ifs_freeform_fleet" or decision.screen == "ifs_freeform_ai"
		or decision.curScreen == "battle_back" or decision.curScreen == "summary_fleet"
	local published, message = ScriptCB_SetMetagameDecision(decision.decisionFaction, main.turnNumber,
		decision.screen, decision.pending and decision.pending.index or 0,
		phase, source, math.floor(math.max(0, remaining) * 1000), ifs_freeform_net_fnBonusMask(), strategic)
	if not published then
		ifs_freeform_net_fnSessionError(message)
	end
end

-- Initialize decision inputs after native entry creates the world and applies the battle result.
function ifs_freeform_net_fnStartDecision(name)
	if not ifs_freeform_net_campaign.state.host then
		return
	end

	local main, saved = ifs_freeform_main, ifs_freeform_net_campaign.saved
	local decision = {
		playerTeam = saved.playerTeam,
		planetSelected = saved.planetNext,
		planetNext = saved.planetNext,
		lastSelected = {},
		lastFleet = {},
		curScreen = saved.curScreen
	}
	for team, planet in pairs(saved.lastSelected) do
		decision.lastSelected[team] = planet
	end
	for team, planet in pairs(saved.lastFleet) do
		decision.lastFleet[team] = planet
	end
	if saved.fleetTurn == main.turnNumber and (name == "ifs_freeform_fleet"
		or saved.curScreen == "battle_back" or saved.curScreen == "summary_fleet") then
		decision.move = {
			turn = saved.fleetTurn,
			source = saved.fleetStart,
			target = saved.fleetNext
		}
	end

	ifs_freeform_net_campaign.decision = decision
	if name == "ifs_freeform_ai"
		and ifs_freeform_net_campaign.state.commanders[decision.playerTeam] >= 0 then
		name = "ifs_freeform_fleet"
	end
	if name == "ifs_freeform_battle_card" then
		decision.screen, decision.decisionFaction = name, decision.playerTeam
		ifs_freeform_net_fnStartBonuses(true)
	else
		ifs_freeform_net_fnAdvanceDecision(name, saved.curScreen)
	end
	ifs_freeform_net_fnPublishDecision()
end

-- Service accepted reveals, committed actions and the native AI once from the active screen.
function ifs_freeform_net_fnUpdateDecision(dt)
	local decision, state = ifs_freeform_net_campaign.decision, ifs_freeform_net_campaign.state
	if not decision or not state.host or ifs_freeform_net_campaign.executing
		or ifs_freeform_net_campaign.diskPending then
		return
	end

	if decision.bonus then
		local bonus, before, changed = decision.bonus, ifs_freeform_net_fnBonusMask(), false
		if bonus.launched then
			return
		end

		for team = 1, 2 do
			local choice = bonus.choices[team]
			if not choice.accepted then
				if state.bonusExpired then
					ifs_freeform_net_fnAcceptBonus(team, 0)
					changed = true
				elseif state.commanders[team] < 0 then
					ifs_freeform_net_fnChooseAIBonus(team)
					changed = true
				end
			end
			if choice.remaining then
				choice.remaining = choice.remaining - dt
				if choice.remaining < 0 then
					choice.done, choice.remaining = true, nil
				end
			end
		end

		ifs_freeform_net_fnFinishBonuses()
		if changed or before ~= ifs_freeform_net_fnBonusMask() then
			ifs_freeform_net_fnPublishDecision()
		end
		return
	end

	if decision.presentation then
		local presentation = decision.presentation
		presentation.remaining = presentation.remaining - dt
		if presentation.remaining <= 0 then
			decision.presentation = nil
			if presentation.continuation == "attack" then
				decision.move = nil
				ifs_freeform_net_fnAdvanceDecision("ifs_freeform_battle_mode")
			elseif presentation.continuation == "next_turn" then
				decision.move = nil
				ifs_freeform_net_fnNextTurn()
			end
			ifs_freeform_net_fnPublishDecision()
		end
		return
	end

	if decision.move and decision.move.remaining then
		decision.move.remaining = decision.move.remaining - dt
		if decision.move.remaining < 0 then
			decision.move.remaining = nil
			ifs_freeform_net_fnAdvanceDecision(decision.move.screen)
			ifs_freeform_net_fnPublishDecision()
		end
		return
	end

	if state.timerExpired then
		ifs_freeform_net_fnExpireTurn()
	end
	if decision.screen == "ifs_freeform_ai" then
		local ai, main = decision.ai, ifs_freeform_main
		local update, selected, funds =
			ai.Update, decision.planetSelected, main.teamResources[1] + main.teamResources[2]
		ifs_freeform_net_fnRunAI(ai.Update, ai, dt)
		if decision.screen ~= "ifs_freeform_ai" or ai.Update ~= update or selected ~= decision.planetSelected
			or funds ~= main.teamResources[1] + main.teamResources[2] then
			ifs_freeform_net_fnPublishDecision()
		end
	end
end

-- Submit only a strategic decision at expiry; Results remain a host-owned native continuation.
function ifs_freeform_net_fnExpireTurn()
	local decision, state = ifs_freeform_net_campaign.decision, ifs_freeform_net_campaign.state
	if decision.expiredDecision == state.decisionId or decision.presentation then
		return
	end
	if decision.screen ~= "ifs_freeform_fleet" and decision.curScreen ~= "battle_back"
		and decision.curScreen ~= "summary_fleet" then
		return
	end

	decision.expiredDecision = state.decisionId
	if decision.pending then
		ifs_freeform_net_fnApplyAction({
			name = "cancel_card",
			faction = decision.decisionFaction
		})
	end
	if decision.screen == "ifs_freeform_battle" then
		ifs_freeform_net_fnApplyAction({
			name = "attack",
			faction = decision.decisionFaction
		})
	else
		if decision.screen == "ifs_freeform_fleet" then
			ifs_freeform_net_fnAdvanceDecision("ifs_freeform_summary")
		end
		ifs_freeform_net_fnApplyAction({
			name = "continue_summary",
			faction = decision.decisionFaction
		})
	end
	ifs_freeform_net_fnPublishDecision()
end
