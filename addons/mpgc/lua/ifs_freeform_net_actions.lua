-- Return the cost of building this faction's next fleet.
function ifs_freeform_net_fnFleetCost(team)
	local count = 0

	-- Contested fleets use 0 and do not count toward the next fleet's cost.
	for planet, faction in pairs(ifs_freeform_main.planetFleet) do
		if faction == team then
			count = count + 1
		end
	end

	return ifs_freeform_fleet_cost[count]
end

-- Apply native movement without selecting a planet or entering a screen on the host's view.
function ifs_freeform_net_fnMoveFleet(team, source, target)
	local decision, main = ifs_freeform_net_campaign.decision, ifs_freeform_main
	local selectedFleet = main.lastFleet[team]

	main:MoveFleet(team, source, target)
	main.lastFleet[team] = selectedFleet
	decision.lastFleet[team] = target
	decision.planetSelected, decision.planetNext = target, target
	decision.move = {
		turn = main.turnNumber,
		source = source,
		target = target,
		remaining = 1
	}
	decision.move.screen = (main.planetTeam[target] == 3 - team or main.planetFleet[target] == 0)
		and "ifs_freeform_battle" or "ifs_freeform_summary"
end

-- Reverse only this turn's unresolved move, retaining the native entity and defender.
function ifs_freeform_net_fnUndoMove(team)
	local decision, main = ifs_freeform_net_campaign.decision, ifs_freeform_main
	local move = decision.move
	if not move or move.turn ~= main.turnNumber then
		return false
	end

	local selectedFleet = main.lastFleet[team]
	main:MoveFleet(team, move.target, move.source)
	main.lastFleet[team] = selectedFleet
	decision.lastFleet[team] = move.source
	decision.planetSelected, decision.planetNext = move.source, move.source
	decision.move = nil
	ifs_freeform_net_fnAdvanceDecision("ifs_freeform_fleet")

	return true
end

-- Apply a campaign action on the host; the bridge authenticates its sender and decision.
function ifs_freeform_net_fnApplyAction(action)
	local decision, main, fleet = ifs_freeform_net_campaign.decision, ifs_freeform_main, ifs_freeform_fleet
	local name, team, screen = action.name, action.faction, decision.screen
	local strategic = screen == "ifs_freeform_fleet"

	if decision.presentation then
		return false, "The committed action is being shown."
	end

	if name ~= "undo_move" and strategic and decision.move and decision.move.remaining then
		return false, "The fleet move is being displayed."
	end

	if name ~= "choose_bonus" and team ~= decision.decisionFaction then
		return false, "The campaign decision changed."
	end

	if name == "recruit" and strategic then
		local item = action.first
		local units = ifs_purchase_team_table[main.teamCode[team]].classes

		-- The visible unit table belongs to the viewer; validate the actor's native side instead.
		local available = false
		for _, unit in ipairs(ifs_purchase_unit_types) do
			if unit == item then
				available = true
			end
		end

		if not units[item] or not available then
			return false, "That troop is unavailable."
		end

		if ifs_purchase_unit_owned[team][item] then
			return false
		end

		if not main:SpendResources(team, ifs_purchase_unit_cost[item]) then
			return false, "There are not enough credits."
		end

		ifs_purchase_unit_owned[team][item] = true
		return true
	elseif name == "buy_card" and strategic and not decision.pending then
		local tech = ifs_purchase_tech_table[action.index]
		if not tech then
			return false, "That bonus card is unavailable."
		end

		local owned = ifs_purchase_tech_cards[team][action.index]
		local cost = tech.cost[owned]
		if not main:SpendResources(team, cost) then
			return false, "There are not enough credits."
		end

		if owned then
			decision.pending = {
				index = action.index,
				paid = cost,
				faction = team
			}
		else
			ifs_purchase_tech_cards[team][action.index] = true
		end

		return true
	elseif name == "place_card" and strategic and decision.pending then
		local slots, pending = ifs_purchase_tech_using[team], decision.pending
		if not slots[action.index] or (slots[action.index] ~= 0 and not action.confirm) then
			return false, "Confirm replacement of the held bonus card."
		end

		slots[action.index], decision.pending = pending.index, nil
		return true
	elseif name == "cancel_card" and strategic and decision.pending then
		local pending = decision.pending
		main:AddResources(pending.faction, pending.paid)
		decision.pending = nil
		return true
	elseif name == "build_fleet" and strategic and not decision.pending then
		if not main.planetTeam[action.first] or not fleet:IsValidBuild(team, action.first) then
			return false
		end

		if not main:SpendResources(team, ifs_freeform_net_fnFleetCost(team)) then
			return false
		end

		local selectedFleet = main.lastFleet[team]
		main:CreateFleet(team, action.first)
		main.lastFleet[team] = selectedFleet
		decision.lastFleet[team] = action.first
		decision.planetSelected, decision.planetNext = action.first, action.first
		ifs_freeform_net_fnCommitPresentation("build", action.first)
		return true
	elseif name == "move_fleet" and strategic and not decision.pending then
		if main.planetFleet[action.first] ~= team or not main.planetDestination[action.first]
			or not fleet:IsValidMove(team, action.first, action.second) then
			return false
		end

		ifs_freeform_net_fnMoveFleet(team, action.first, action.second)
		return true
	elseif name == "undo_move"
		and (strategic or screen == "ifs_freeform_battle" or screen == "ifs_freeform_summary") then
		if decision.curScreen == "summary_result" then
			ifs_freeform_net_fnAdvanceDecision("ifs_freeform_result")
			return true
		end

		if decision.curScreen == "battle_noback" then
			return false
		end

		if ifs_freeform_net_fnUndoMove(team) then
			return true
		end

		if screen == "ifs_freeform_summary" then
			ifs_freeform_net_fnAdvanceDecision("ifs_freeform_fleet")
			return true
		end
	elseif name == "pass" and strategic and not decision.pending then
		ifs_freeform_net_fnAdvanceDecision("ifs_freeform_summary")
		return true
	elseif name == "attack" and screen == "ifs_freeform_battle" then
		if decision.move then
			ifs_freeform_net_fnCommitPresentation("move", decision.move.source, "attack")
		else
			ifs_freeform_net_fnAdvanceDecision("ifs_freeform_battle_mode")
		end

		return true
	elseif name == "decline_attack" and screen == "ifs_freeform_battle" then
		if decision.curScreen ~= "battle_back" then
			return false, "This encounter cannot be cancelled."
		end

		return ifs_freeform_net_fnUndoMove(team)
	elseif name == "choose_mode" and screen == "ifs_freeform_battle_mode" then
		if not decision.modes[action.first] then
			return false, "That battle mode is unavailable."
		end

		main:SetLaunchMission(decision.modes[action.first])
		ifs_freeform_net_fnAdvanceDecision("ifs_freeform_battle_card")
		return true
	elseif name == "choose_bonus" and screen == "ifs_freeform_battle_card" then
		return ifs_freeform_net_fnAcceptBonus(team, action.index)
	elseif name == "continue_result" and screen == "ifs_freeform_result" then
		local target, attacker = decision.planetNext, decision.playerTeam
		decision.move = nil
		if main.planetFleet[target] == attacker and main.planetTeam[target] == 3 - attacker then
			ifs_freeform_net_fnAdvanceDecision("ifs_freeform_battle", "battle_noback")
		else
			ifs_freeform_net_fnAdvanceDecision("ifs_freeform_summary", "summary_result")
		end

		return true
	elseif name == "continue_summary" and screen == "ifs_freeform_summary" then
		if decision.move then
			ifs_freeform_net_fnCommitPresentation("move", decision.move.source, "next_turn")
		else
			ifs_freeform_net_fnNextTurn()
		end

		return true
	end

	return false, "That action is unavailable at this campaign step."
end
