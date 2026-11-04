-- Preserve nil result slots from original callbacks.
local function net_battle_results(...) return arg end

-- Read a faction's bit without depending on an additional Lua bit library.
function ifs_freeform_net_fnBonusPending(faction)
	local mask = ifs_freeform_net_campaign.state.bonusPendingMask or 0
	return (faction == 1 or faction == 2) and math.mod(math.floor(mask / faction), 2) == 1
end

-- Return the accepted participant's native roster name.
function ifs_freeform_net_fnParticipantName(slot)
	for _, participant in ipairs(ifs_freeform_net_campaign.state.participants or {}) do
		if participant.slot == slot then
			return participant.name
		end
	end

	return ""
end

-- Use AI when the faction has no human commander.
function ifs_freeform_net_fnCommanderName(faction)
	local slot = ifs_freeform_net_campaign.state.commanders[faction]
	return slot and slot >= 0 and ifs_freeform_net_fnParticipantName(slot) or "AI"
end

-- Show the current commander and faction with the local faction's credits in the original header.
function ifs_freeform_net_fnUpdateHud(player)
	local state = ifs_freeform_net_campaign.state
	local faction, main = state.decisionFaction, ifs_freeform_main
	if state.decisionScreen == "ifs_freeform_battle_card" then
		faction = state.localFaction
	end

	if state.commanders and (faction == 1 or faction == 2) then
		IFText_fnSetUString(player.name, ScriptCB_tounicode(ifs_freeform_net_fnCommanderName(faction)))
		IFImage_fnSetTexture(player.icon, "seal_" .. main.teamCode[faction])
		local r, g, b = main:GetTeamColor(faction)
		IFObj_fnSetColor(player.name, r, g, b)
		IFObj_fnSetColor(player.icon, r, g, b)
	end

	local credits = ""
	if state.localFaction == 1 or state.localFaction == 2 then
		credits = ScriptCB_usprintf("ifs.freeform.credits", ScriptCB_tounicode(main:GetResources(state.localFaction)))
	end
	IFText_fnSetUString(player.resources, credits)

	if player.mpgc_turn then
		IFText_fnSetUString(player.mpgc_turn, ScriptCB_tounicode("Turn: " .. tostring(ifs_freeform_main.turnNumber)))
		IFObj_fnSetVis(player.mpgc_turn, true)
	end

	if player.mpgc_timer then
		local bonus = state.decisionScreen == "ifs_freeform_battle_card"
		local remaining = bonus and state.bonusRemainingMs or state.timerRemainingMs
		local visible = bonus and ifs_freeform_net_fnBonusPending(state.localFaction)
			or (not bonus and state.timerEnabled and remaining and remaining >= 0)
		local seconds = math.ceil(math.max(0, remaining or 0) * 0.001)
		IFText_fnSetUString(player.mpgc_timer,
			ScriptCB_tounicode((bonus and "Bonus: " or "Timer: ")
				.. string.format("%d:%02d", math.floor(seconds / 60), math.mod(seconds, 60))))
		IFObj_fnSetVis(player.mpgc_timer, visible)
	end
end

-- Every registered screen browses the accepted local faction.
function ifs_freeform_net_fnSetViewFaction()
	local faction = ifs_freeform_net_campaign.state.localFaction
	if (faction == 1 or faction == 2) and ifs_freeform_main.playerTeam ~= faction then
		ifs_freeform_main:SetActiveTeam(faction)
	end
end

-- Check host ownership for Results Done and accepted commander ownership for other decisions.
function ifs_freeform_net_fnCanCommand()
	local state = ifs_freeform_net_campaign.state
	local faction = state and state.decisionFaction
	if state and state.decisionScreen == "ifs_freeform_battle_card" then
		faction = state.localFaction
		if not ifs_freeform_net_fnBonusPending(faction) then
			return false
		end
		if ifs_freeform_main.activeBonus[faction] then
			return false
		end

		local context = ifs_freeform_net_campaign.decision and ifs_freeform_net_campaign.decision.bonus
		context = context and context.choices[faction]
		if context and context.accepted then
			return false
		end

		local card = ifs_freeform_battle_card
		if card.mpgc_acceptedDecision == state.decisionId then
			return false
		end
	end

	local available = ifs_freeform_net_campaign.active and not ifs_freeform_net_campaign.failed
		and state and not ifs_freeform_net_campaign.requested
		and (not state.presentationPhase or state.presentationPhase == "")
		and not (ifs_freeform_net_campaign.decision and ifs_freeform_net_campaign.decision.presentation)
		and (state.phase == "strategic" or state.phase == "preparing" or state.phase == "returning")
	if not available then
		return false
	end

	-- Results Done belongs to the host; the encounter still retains its attacking faction.
	if state.decisionScreen == "ifs_freeform_result" then
		return state.host
	end
	return state.commanders and state.localFaction == faction and state.commanders[faction] == state.localSlot
end

-- Submit one finite choice; native authority rechecks membership, action ownership and decision identity.
function ifs_freeform_net_fnRequest(name, first, second, index, confirm)
	if not ifs_freeform_net_fnCanCommand() then
		return false
	end
	if name ~= "undo_move" and ifs_freeform_net_fnMovePending() then
		return false
	end

	local accepted, message = ScriptCB_SubmitMetagameAction(ifs_freeform_net_campaign.state.decisionId,
		name, first or "", second or "", index or 0, confirm or false)
	if accepted then
		local faction = ifs_freeform_net_campaign.state.localFaction
		-- Retain the submitted choice after its native inventory slot is consumed.
		ifs_freeform_net_campaign.requested = {
			name = name,
			first = first,
			index = index,
			screen = gCurScreenTable,
			faction = faction,
			decisionId = ifs_freeform_net_campaign.state.decisionId,
			owned = name == "buy_card" and ifs_purchase_tech_cards[faction][index],
			bonus = name == "choose_bonus" and index and index > 0
				and (ifs_freeform_main.activeBonus[faction]
					or gCurScreenTable.useActive[gCurScreenTable.selected].bonus),
			previousBonus = name == "choose_bonus" and (ifs_freeform_main.activeBonus[faction]
				or (gCurScreenTable.mpgc_acceptedDecision == ifs_freeform_net_campaign.state.decisionId
					and gCurScreenTable.mpgc_acceptedBonus)),
		}
	end

	return accepted, message
end

-- Play the stock private feedback after the submitting client's result is accepted.
function ifs_freeform_net_fnActionFeedback(request, accepted)
	local name, screen = request.name, request.screen
	if name == "attack" or name == "choose_mode" or name == "continue_result" or name == "continue_summary"
		or name == "decline_attack" or name == "undo_move" then
		local back = name == "decline_attack" or name == "undo_move"
		ifelm_shellscreen_fnPlaySound(
			accepted and (back and screen.cancelSound or screen.acceptSound) or screen.cancelSound)
		return
	end

	if name ~= "recruit" and name ~= "buy_card" and name ~= "place_card"
		and name ~= "cancel_card" and name ~= "choose_bonus" then
		return
	end
	if request.faction ~= ifs_freeform_net_campaign.state.localFaction then
		return
	end
	if not accepted then
		ifelm_shellscreen_fnPlaySound(screen.cancelSound)
		return
	end
	if name == "choose_bonus" and not request.bonus then
		return
	end

	ifelm_shellscreen_fnPlaySound(name == "cancel_card" and screen.cancelSound or screen.acceptSound)
	local main, side = ifs_freeform_main, ifs_freeform_main.teamCode[request.faction]
	if name == "recruit" then
		main:PlayVoice(string.format(ifs_purchase_unit_bought_sound, side, ifs_purchase_unit_name[request.first]))
	elseif name == "buy_card" then
		local tech = ifs_purchase_tech_table[request.index]
		main:PlayVoice(string.format(ifs_purchase_tech_bought_sound[request.owned or false], side,
			request.owned and tech.bonus or tech.name))
	elseif name == "choose_bonus" and request.bonus ~= request.previousBonus then
		main:PlayVoice(string.format(ifs_battle_card_play_us_sound, side, request.bonus))
	end
end

-- Retain the accepted own-team card while the host runs its native display delay.
function ifs_freeform_net_fnShowAcceptedBonus(this)
	local state, main = ifs_freeform_net_campaign.state, ifs_freeform_main
	if state.localFaction < 1 then
		return
	end

	local bonus = main.activeBonus[state.localFaction]
	if this.mpgc_acceptedDecision == state.decisionId then
		bonus = this.mpgc_acceptedBonus or bonus
	end
	if not bonus then
		return
	end

	local selected
	for index, item in ipairs(this.useActive) do
		if item.bonus == bonus then
			selected = index
		end
	end
	if not selected then
		for index, tech in ipairs(ifs_purchase_tech_table) do
			if tech.bonus == bonus then
				for _, item in ipairs(this.useItems) do
					IFObj_fnSetVis(item, false)
				end
				-- The registered model belongs to this faction; accepted metadata survives inventory consumption.
				local item = this.useItems[1]
				item.using, item.name, item.bonus, item.weight = index, tech.name, bonus, 1
				IFModel_fnSetMsh(item, tech.mesh)
				IFModel_fnSetTranslation(item, 0, 0, -25)
				IFObj_fnSetVis(item, true)
				this.selected, this.rollover, this.useActive = nil, nil, { item }
				selected = 1
				break
			end
		end
	end

	this:SetSelected(selected)
	this:UpdateAction()
	ifs_freeform_SetButtonVis(this, "accept", false)
	ifs_freeform_SetButtonVis(this, "misc", false)
	return true
end

-- Submit explicit fleet inputs while the original callback retains local selection and navigation.
function ifs_freeform_net_fnFleetRequest(name, team, first, second)
	if team ~= ifs_freeform_net_campaign.state.decisionFaction then
		return false
	end
	if name == "BuildFleet" then
		return ifs_freeform_net_fnRequest("build_fleet", first)
	end
	if name == "AttemptMove" then
		return ifs_freeform_net_fnRequest("move_fleet", first, second)
	end
	return ifs_freeform_net_fnRequest("undo_move")
end

-- Refresh campaign controls and route the local view to the accepted decision.
function ifs_freeform_net_fnShowDecision(this)
	if ifs_freeform_net_campaign.saving then
		return
	end

	local state = ifs_freeform_net_campaign.state
	local canCommand = ifs_freeform_net_fnCanCommand()

	if this == ifs_freeform_fleet then
		this:UpdateFleetCost()
		this:UpdateAction()
	elseif this == ifs_freeform_purchase_unit or this == ifs_freeform_purchase_tech then
		if ifs_freeform_net_campaign.refreshPending then
			ifs_freeform_net_fnRefreshPurchases(this,
				ifs_freeform_net_campaign.refreshSide or ifs_freeform_main.playerSide)
		elseif this == ifs_freeform_purchase_unit then
			this:UpdateAction(this.purchaseItems[this.selected])
		elseif this.focus == ifs_purchase_tech_focus_using then
			this:UpdateActionUsing()
		else
			this:UpdateActionCarousel()
		end
	elseif this == ifs_freeform_focus and ifs_freeform_net_campaign.refreshPending then
		ifs_freeform_main:UpdatePlanetInfo(this.info, ifs_freeform_main.planetSelected)
	elseif state.decisionScreen and this == _G[state.decisionScreen]
		and not (this == ifs_freeform_battle_card and state.localFaction < 1) then
		if this == ifs_freeform_battle_card then
			ifs_freeform_SetButtonVis(this, "accept", canCommand and this.selected)
			ifs_freeform_SetButtonVis(this, "misc", canCommand)
			ifs_freeform_net_fnShowAcceptedBonus(this)
		end

		-- These native steps name their required Accept button without restoring its visibility.
		if this == ifs_freeform_summary or this == ifs_freeform_battle or this == ifs_freeform_result
			or this == ifs_freeform_battle_mode then
			ifs_freeform_SetButtonVis(this, "accept", canCommand)
		elseif not canCommand then
			ifs_freeform_SetButtonVis(this, "accept", false)
		end
		if not canCommand then
			ifs_freeform_SetButtonVis(this, "misc", false)
		end
	end

	if this == ifs_freeform_battle then
		ifs_freeform_SetButtonVis(this, "back",
			canCommand and ifs_freeform_net_campaign.saved.curScreen == "battle_back")
	elseif this == ifs_freeform_summary then
		ifs_freeform_SetButtonVis(this, "back", canCommand)
	end
	ifs_freeform_net_campaign.refreshPending, ifs_freeform_net_campaign.refreshSide = nil, nil
	if this.player then
		ifs_freeform_net_fnUpdateHud(this.player)
	end

	if this == ifs_freeform_purchase_tech and (state.pendingCard or 0) > 0
		and state.localFaction == state.decisionFaction then
		local slot = this.useItems.cursor.position or 1
		if not this.mpgc_pending then
			slot = 1
			for index, card in ipairs(ifs_purchase_tech_using[state.localFaction]) do
				if card == 0 then
					slot = index
					break
				end
			end
		end

		this.mpgc_pending = true
		this.useItems.cursor.index = state.pendingCard
		IFModel_fnSetMsh(this.useItems.cursor.card, ifs_purchase_tech_table[state.pendingCard].mesh)
		IFObj_fnSetColor(this.useItems.cursor.card, 255, 255, 255)
		this:SetCursor(slot)
		this:FocusUsing()
	elseif this == ifs_freeform_purchase_tech then
		this.mpgc_pending = nil
		if this.focus == ifs_purchase_tech_focus_using then
			this:FocusCarousel()
		end
	end

	local screen = state.decisionScreen
	if ifs_freeform_net_campaign.presentation then
		return
	end

	-- Bonus choices stay with their faction, including after a card is accepted.
	if screen == "ifs_freeform_battle_card" and not ifs_freeform_net_fnBonusPending(state.localFaction) then
		if this == ifs_freeform_battle_card then
			-- Keep a confirmed own-team card visible while the other faction finishes its choice.
			ifs_freeform_SetButtonVis(this, "accept", false)
			ifs_freeform_SetButtonVis(this, "misc", false)
		end
		ifs_freeform_net_campaign.shownDecision = nil
		return
	end

	-- Human fleet positions remain reversible until Continue or Attack commits the action.
	if (screen == "ifs_freeform_summary" or screen == "ifs_freeform_battle")
		and ifs_freeform_net_fnReversibleMove() and not canCommand then
		return
	end
	if screen and screen ~= "ifs_freeform_fleet" and screen ~= "ifs_freeform_ai"
		and state.decisionId ~= ifs_freeform_net_campaign.shownDecision and _G[screen] then
		if this ~= ifs_freeform_menu and this ~= ifs_mp_lobby then
			ifs_freeform_net_campaign.shownDecision = state.decisionId
			local main = ifs_freeform_main
			main:SelectPlanet(nil, ifs_freeform_net_campaign.saved.planetNext)
			ifs_freeform_net_fnSetViewFaction()
			if this == _G[screen] then
				this:Enter(true)
			else
				ScriptCB_PushScreen(screen)
			end
		end
	elseif screen == "ifs_freeform_fleet" then
		if ifs_freeform_net_campaign.shownDecision then
			ScriptCB_PopScreen("ifs_freeform_main")
			ScriptCB_PushScreen("ifs_freeform_fleet")
		end
		ifs_freeform_net_campaign.shownDecision = nil
	end
end

-- Preserve all original decision presentation and run its mutations only in the host context.
function ifs_freeform_net_fnWrapDecisionScreen(this, name)
	if name == "ifs_freeform_result" then
		local previous_result = this.UpdateResult
		this.UpdateResult = function(this, container, team, ...)
			local result = net_battle_results(previous_result(this, container, team, unpack(arg, 1, arg.n)))
			if ifs_freeform_net_campaign.active then
				IFText_fnSetString(container.name, ifs_freeform_main.teamName[team])
			end

			return unpack(result, 1, result.n)
		end
	end

	local previous_enter, previous_update, previous_accept = this.Enter, this.Update, this.Input_Accept
	this.Enter = function(this, ...)
		if not ifs_freeform_net_campaign.active or ifs_freeform_net_campaign.executing then
			return previous_enter(this, unpack(arg, 1, arg.n))
		end
		if name == "ifs_freeform_battle_card" and ifs_freeform_net_campaign.state.localFaction < 1 then
			ifs_freeform_net_fnSetViewFaction()
			ScriptCB_PopScreen("ifs_freeform_main")
			return ScriptCB_PushScreen("ifs_freeform_fleet")
		end

		local next, bonus, accept = this.Next, this.AcceptBonus, this.Input_Accept
		local main, saved, state = ifs_freeform_main, ifs_freeform_net_campaign.saved, ifs_freeform_net_campaign.state
		local team, joystick, other = main.playerTeam, main.joystick, main.joystick_other

		-- Native encounter captions/results read the attacker; inventories still render the local faction.
		if name == "ifs_freeform_battle" or name == "ifs_freeform_result" then
			main:SetActiveTeam(saved.playerTeam)
		elseif name == "ifs_freeform_battle_mode" then
			main:SetActiveTeam(state.decisionFaction)
		end
		main.joystick = main.startController
		if name == "ifs_freeform_battle_mode" then
			-- Authority already chose the deciding faction; presentation must not switch it again.
			main.joystick_other, this.originalTeam = nil, nil
		end
		if name == "ifs_freeform_battle_card" then
			ifs_freeform_net_fnSetViewFaction()
		end

		-- Stock Enter can skip modes or consume a bonus immediately; render without advancing the shared decision.
		this.Next, this.AcceptBonus, this.Input_Accept = function() end, function() end, function() end
		local result = net_battle_results(pcall(previous_enter, this, unpack(arg, 1, arg.n)))

		this.Next, this.AcceptBonus, this.Input_Accept = next, bonus, accept
		main:SetActiveTeam(team)
		main.joystick, main.joystick_other = joystick, other
		if name == "ifs_freeform_battle" then
			this.allowBack = saved.curScreen == "battle_back"
		end
		-- The host owns reveal deadlines; stock display timers must not call their continuation locally.
		this.displayTimer = nil

		if not result[1] then
			error(result[2])
		end

		if name == "ifs_freeform_battle_card" then
			if this.mpgc_acceptedDecision ~= ifs_freeform_net_campaign.state.decisionId then
				this.mpgc_acceptedDecision, this.mpgc_acceptedBonus = nil, nil
			end
			ifs_freeform_net_fnShowAcceptedBonus(this)
		end

		return unpack(result, 2, result.n)
	end

	this.Input_Accept = function(this, joystick, ...)
		if not ifs_freeform_net_campaign.active or ifs_freeform_net_campaign.executing then
			return previous_accept(this, joystick, unpack(arg, 1, arg.n))
		end
		if name ~= "ifs_freeform_battle_mode" and this.CurButton ~= "_accept"
			and this.CurButton ~= "_next" and this.CurButton ~= "_back" then
			return previous_accept(this, joystick, unpack(arg, 1, arg.n))
		end

		if name == "ifs_freeform_battle" then
			return ifs_freeform_net_fnRequest(this.CurButton == "_back" and "decline_attack" or "attack")
		elseif name == "ifs_freeform_battle_mode" then
			local mode = this.CurButton == "_accept" and this.PrevButton or this.CurButton
			return ifs_freeform_net_fnRequest("choose_mode", mode)
		elseif name == "ifs_freeform_battle_card" then
			local item = this.selected and this.useActive[this.selected]
			return ifs_freeform_net_fnRequest("choose_bonus", "", "",
				this.CurButton == "_next" and 0 or (item and item.slot or 0))
		elseif name == "ifs_freeform_result" then
			return ifs_freeform_net_fnRequest("continue_result")
		elseif name == "ifs_freeform_summary" then
			return ifs_freeform_net_fnRequest(this.CurButton == "_back" and "undo_move" or "continue_summary")
		end
	end

	this.Update = function(this, ...)
		if ifs_freeform_net_campaign.active and not ifs_freeform_net_campaign.executing then
			this.displayTimer = nil
		end
		return previous_update(this, unpack(arg, 1, arg.n))
	end

	if name == "ifs_freeform_battle" or name == "ifs_freeform_summary" then
		local previous_back = this.Input_Back
		this.Input_Back = function(this, ...)
			if ifs_freeform_net_campaign.active and not ifs_freeform_net_campaign.executing then
				return ifs_freeform_net_fnRequest(name == "ifs_freeform_battle" and "decline_attack" or "undo_move")
			end
			return previous_back(this, unpack(arg, 1, arg.n))
		end
	end

	if name == "ifs_freeform_battle_card" then
		-- Native selection stays on the accepted card until the host advances this step.
		local previous_selected = this.SetSelected
		this.SetSelected = function(this, selected, ...)
			if ifs_freeform_net_campaign.active and not ifs_freeform_net_campaign.executing then
				local state = ifs_freeform_net_campaign.state
				local bonus = ifs_freeform_main.activeBonus[state.localFaction]
				if this.mpgc_acceptedDecision == state.decisionId then
					bonus = this.mpgc_acceptedBonus or bonus
				end
				if bonus and (not selected or this.useActive[selected].bonus ~= bonus) then
					return
				end
			end
			return previous_selected(this, selected, unpack(arg, 1, arg.n))
		end
	end

	ifs_freeform_net_fnWrapSessionUpdate(this)
end

-- AI replaces Update with named phases; keep those exact functions and service from the active view.
function ifs_freeform_net_fnWrapAI(this)
	local previous_enter = this.Enter
	this.mpgc_enter = previous_enter

	-- Service the session while native AI phase callbacks remain on the actor record.
	local function update_view(this, dt)
		ifs_freeform_net_fnPoll(this)
		ifs_freeform_net_fnUpdateDecision(dt)
		if not ifs_freeform_net_fnUpdatePresentation(this, dt) then
			gIFShellScreenTemplate_fnUpdate(this, dt)
		end
		ifs_freeform_net_fnUpdateChat(this, dt)
	end

	this.Enter = function(this, ...)
		if ifs_freeform_net_campaign.active and not ifs_freeform_net_campaign.executing then
			this.Update = update_view
			return
		end
		return previous_enter(this, unpack(arg, 1, arg.n))
	end

	this.Update = update_view
	ifs_freeform_net_fnWrapSessionError(this)
end
