-- Keep presentation fields separate from each machine's stock camera and selection.
local net_view_fields = {
	"planetSelected", "planetNext",
}

-- Preserve nil result slots while restoring temporary engine callbacks.
local function net_battle_results(...) return arg end

-- Native move display accepts Undo while other finite choices wait for its next screen.
function ifs_freeform_net_fnMovePending()
	local decision, state = ifs_freeform_net_campaign.decision, ifs_freeform_net_campaign.state
	if decision and decision.screen == "ifs_freeform_fleet" and decision.move and decision.move.remaining then
		return true
	end

	local saved = ifs_freeform_net_campaign.saved
	return state.decisionScreen == "ifs_freeform_fleet" and saved
		and saved.fleetTurn == ifs_freeform_main.turnNumber and saved.fleetStart ~= nil
end

-- Locate this turn's unresolved move, excluding postbattle continuations.
function ifs_freeform_net_fnReversibleMove()
	local decision, state, saved =
		ifs_freeform_net_campaign.decision, ifs_freeform_net_campaign.state, ifs_freeform_net_campaign.saved
	local screen, team, turn, source, target, step

	if state.host and decision then
		screen, team, step = decision.screen, decision.playerTeam, decision.curScreen
		local move = decision.move
		if move then
			turn, source, target = move.turn, move.source, move.target
		end
	elseif saved then
		screen, team, step = state.decisionScreen, saved.playerTeam, saved.curScreen
		turn, source, target = saved.fleetTurn, saved.fleetStart, saved.fleetNext
	end

	if step == "summary_result" or step == "battle_noback" then
		return
	end

	if (screen == "ifs_freeform_fleet" or screen == "ifs_freeform_summary" or screen == "ifs_freeform_battle")
		and turn == ifs_freeform_main.turnNumber and source and target and source ~= target then
		return team, source, target
	end
end

-- Return a held model to its accepted position unless native Undo or removal already changed it.
function ifs_freeform_net_fnReleaseFleetModel()
	local held, main = ifs_freeform_net_campaign.heldFleet, ifs_freeform_main
	if held and main.fleetPtr[held.team][held.target] == held.pointer then
		SetEntityMatrix(held.pointer, main.modelMatrix[held.target][held.team])
	end

	ifs_freeform_net_campaign.heldFleet = nil
end

-- Show a reversible move at its starting planet for everyone except its commander.
function ifs_freeform_net_fnDrawFleets(callback, main, models, ...)
	if not ifs_freeform_net_campaign.active or not ifs_freeform_net_campaign.initialized then
		return callback(main, unpack(arg, 1, arg.n))
	end

	local team, source, target = ifs_freeform_net_fnReversibleMove()
	local state = ifs_freeform_net_campaign.state
	if ifs_freeform_net_campaign.presentation or (team and state.commanders[team] == state.localSlot) then
		team = nil
	end

	local pointer = team and main.fleetPtr[team][target]
	local held = ifs_freeform_net_campaign.heldFleet
	if held and (not pointer or held.pointer ~= pointer or held.team ~= team or held.target ~= target) then
		ifs_freeform_net_fnReleaseFleetModel()
	end

	if not pointer then
		return callback(main, unpack(arg, 1, arg.n))
	end

	if models then
		ifs_freeform_net_campaign.heldFleet = {
			team = team,
			target = target,
			pointer = pointer
		}
		SetEntityMatrix(pointer, main.modelMatrix[source][team])
	end

	local pointers, drawing = main.fleetPtr, {}
	for planet, entity in pairs(pointers[team]) do
		drawing[planet] = entity
	end

	drawing[target], drawing[source] = nil, pointer
	main.fleetPtr = { pointers[1], pointers[2] }
	main.fleetPtr[team] = drawing

	local result = net_battle_results(pcall(callback, main, unpack(arg, 1, arg.n)))

	main.fleetPtr = pointers

	if not result[1] then
		error(result[2])
	end

	return unpack(result, 2, result.n)
end

-- Apply observer fleet positions only while native drawing and model visibility read the fleet table.
function ifs_freeform_net_fnWrapFleetDrawing(main)
	local previous_zoom, previous_icons = main.UpdateZoom, main.DrawFleetIcons

	main.UpdateZoom = function(this, ...)
		return ifs_freeform_net_fnDrawFleets(previous_zoom, this, true, unpack(arg, 1, arg.n))
	end

	main.DrawFleetIcons = function(this, ...)
		return ifs_freeform_net_fnDrawFleets(previous_icons, this, false, unpack(arg, 1, arg.n))
	end
end

-- Reconcile accepted fleet tables through the same entity helpers used by native moves and battles.
function ifs_freeform_net_fnRestoreFleets(previous)
	local main, accepted = ifs_freeform_main, ifs_freeform_main.planetFleet
	main.planetFleet = previous

	for team = 1, 2 do
		local removed, added = {}, {}

		-- A fleet value of 0 contains both factions, so compare each faction's presence separately.
		for planet, faction in pairs(previous) do
			if (faction == team or faction == 0) and accepted[planet] ~= team and accepted[planet] ~= 0 then
				table.insert(removed, planet)
			end
		end

		for planet, faction in pairs(accepted) do
			if (faction == team or faction == 0) and previous[planet] ~= team and previous[planet] ~= 0 then
				table.insert(added, planet)
			end
		end

		-- Reuse the entity for one relocation; rebuild entities when several positions change.
		if table.getn(removed) == 1 and table.getn(added) == 1 then
			main:MoveFleet(team, removed[1], added[1])
		else
			for _, planet in ipairs(removed) do
				main:DestroyFleet(team, planet)
			end

			for _, planet in ipairs(added) do
				main:CreateFleet(team, planet)
			end
		end
	end

	main.planetFleet = accepted
end

-- Display an irreversible human action before its original continuation advances the campaign.
function ifs_freeform_net_fnCommitPresentation(phase, source, continuation)
	ifs_freeform_net_campaign.decision.presentation = {
		phase = phase,
		source = source,
		remaining = 3,
		continuation = continuation,
	}
end

-- Describe the stock AI phase or a committed human action with the accepted campaign snapshot.
function ifs_freeform_net_fnPresentationState()
	local decision = ifs_freeform_net_campaign.decision
	if not decision then
		return "", "", 0
	end

	if decision.presentation then
		local presentation = decision.presentation
		return presentation.phase, presentation.source, presentation.remaining
	end

	local ai = decision.ai
	if decision.screen == "ifs_freeform_ai" and ai then
		if ai.Update == ifs_freeform_ai.UpdateBuildFleet then
			local preview = not ifs_freeform_main.planetFleet[decision.planetSelected]
			return "build", decision.planetSelected, ai.displayTime + (preview and 1 or 0)
		elseif ai.Update == ifs_freeform_ai.UpdateMoveFleet then
			local preview = decision.planetSelected ~= decision.planetNext
			return "move", decision.planetSelected, ai.displayTime + (preview and 1 or 0)
		end
	end

	return "", "", 0
end

-- Follow only committed actions; the ordinary camera and provisional selection return afterwards.
function ifs_freeform_net_fnSyncPresentation(state, this)
	if not ifs_freeform_net_campaign.initialized then
		return
	end

	local phase = state.presentationPhase or ""
	local presentation, main = ifs_freeform_net_campaign.presentation, ifs_freeform_main
	if phase ~= "" and state.commanders[ifs_freeform_net_campaign.saved.playerTeam] == state.localSlot then
		phase = ""
	end

	if presentation and (phase == "" or presentation.id ~= state.decisionId) then
		local previousSide = main.playerSide
		for _, field in ipairs(net_view_fields) do
			main[field] = presentation.view[field]
		end

		ifs_freeform_net_fnSetViewFaction()
		if previousSide ~= main.playerSide then
			ifs_freeform_net_fnRefreshPurchases(this, previousSide)
		end

		main.planetSelected = nil
		main:SelectPlanet(nil, presentation.view.planetSelected)
		main.planetNext = presentation.view.planetNext
		main:SetZoom(presentation.zoom)
		ifs_freeform_purchase_unit:SetActiveSide()
		ifs_freeform_net_campaign.presentation = nil
	end

	if phase == "" then
		return
	end

	if ifs_freeform_net_campaign.presentation then
		if state.campaign then
			presentation.remaining = math.min(presentation.remaining, state.presentationRemainingMs * 0.001)
		end

		return
	end

	local view = {}
	for _, field in ipairs(net_view_fields) do
		view[field] = main[field]
	end

	local zoom = GetMapCameraZoom()
	presentation = {
		id = state.decisionId,
		phase = phase,
		source = state.presentationSource,
		target = ifs_freeform_net_campaign.saved.planetNext,
		team = ifs_freeform_net_campaign.saved.playerTeam,
		remaining = state.presentationRemainingMs * 0.001,
		view = view,
		zoom = zoom,
	}
	ifs_freeform_net_campaign.presentation = presentation
	main.planetSelected = nil
	main:SelectPlanet(nil, presentation.source)
	main.planetNext = presentation.target
	main:SetZoom(1)
	ifs_freeform_net_fnSetViewFaction()
end

-- Draw the committed fleet action using the game's galaxy drawing helpers.
function ifs_freeform_net_fnUpdatePresentation(this, dt)
	local presentation, main = ifs_freeform_net_campaign.presentation, ifs_freeform_main
	if not ifs_freeform_net_campaign.active or not ifs_freeform_net_campaign.initialized or not presentation then
		return false
	end

	ifs_freeform_net_fnReleaseFleetModel()
	presentation.remaining = math.max(0, presentation.remaining - dt)
	local source, target, team = presentation.source, presentation.target, presentation.team

	-- Keep the final second for the result; earlier frames show the build or move preview.
	local preview = presentation.remaining > 1 and (presentation.phase == "build" or source ~= target)
	local selected, nextPlanet = main.planetSelected, main.planetNext
	main.planetSelected, main.planetNext = preview and source or target, target
	if not preview and not presentation.result then
		presentation.result = true
		MoveCameraToEntity(target)
	end

	local fleets, pointers = main.planetFleet, main.fleetPtr
	local drawing, teamPointers = {}, {}

	-- Supply copied fleet and pointer tables only to drawing, then restore the accepted tables.
	for planet, faction in pairs(fleets) do
		drawing[planet] = faction
	end

	for planet, pointer in pairs(pointers[team]) do
		teamPointers[planet] = pointer
	end

	if preview and pointers[team][target] then
		teamPointers[target] = nil

		-- A fleet value of 0 means both factions; remove only the moving faction from the destination.
		drawing[target] = fleets[target] == 0 and 3 - team or nil
		if presentation.phase == "move" then
			teamPointers[source] = pointers[team][target]
			drawing[source] = fleets[source] and 0 or team
		end
	end

	main.planetFleet, main.fleetPtr = drawing, { pointers[1], pointers[2] }
	main.fleetPtr[team] = teamPointers

	local result = net_battle_results(pcall(function()
		gIFShellScreenTemplate_fnUpdate(this, dt)
		main:UpdateZoom()
		if presentation.phase == "move" then
			main:DrawLanes(source, target, 1)
		else
			main:DrawLanes("", "", 1)
		end

		main:DrawPlanetIcons()
		main:DrawPortIcons(team)
		main:DrawFleetIcons(main.planetSelected, team)
		if preview then
			main:DrawFleetIcon(target, team, true, true)
		end
	end))

	main.planetFleet, main.fleetPtr = fleets, pointers
	main.planetSelected, main.planetNext = selected, nextPlanet

	if not result[1] then
		error(result[2])
	end

	if preview and pointers[team][target] then
		SetProperty(pointers[team][target], "IsVisible", false)
	end

	if this.player then
		ifs_freeform_net_fnUpdateHud(this.player)
	end

	return true
end
