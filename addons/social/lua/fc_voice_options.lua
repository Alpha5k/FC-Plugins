local fc_voice_options_Sensitivity = {-30, -33, -37, -40, -43, -46, -49, -52, -55, -58, -61}

-- Retain callback return counts, including trailing nil values.
local function fc_voice_options_Results(...)
	return arg
end

-- Add only the new rows; existing controls retain their native and Remaster structure.
local function fc_voice_options_AddRows(form, elements, width)
	local container = NewIFContainer {}
	local layout = {
		yTop = form.layout.yTop,
		yHeight = form.layout.yHeight,
		ySpacing = 0,
		xSpacing = form.layout.xSpacing,
		width = width * 2,
		font = form.layout.font,
		flashy = 0,
		bRightJustifyText = 1,
		elements = elements,
	}
	Form_CreateVertical(container, layout)

	for index, element in ipairs(elements) do
		local tag = element.tag
		table.insert(form.layout.elements, element)
		table.insert(form.layout.buttonlist, layout.buttonlist[index])
		form.elements[tag] = container.form.elements[tag]
		for _, group in ipairs({"text", "buttons", "sliders", "radiobuttons", "dropdowns"}) do
			form[group][tag] = container.form[group][tag]
		end
	end
end

-- Apply local echo explicitly; the stock screen otherwise enables it on entry.
local function fc_voice_options_Test(this, enabled)
	this.fcVoiceTest = enabled and this.bShellMode and 1 or nil
	ScriptCB_SetVoiceLocalEchoEnable(this.fcVoiceTest and 1 or 0)
	RoundIFButtonLabel_fnSetUString(this.formcontainer.form.buttons.microphonetest,
		ScriptCB_tounicode(this.fcVoiceTest and "Stop Test" or "Start Test"))
end

-- Repaint voice rows from the profile table used by stock reset, cancel and save.
local function fc_voice_options_Update(this)
	local form = this.formcontainer.form
	local prefs = ScriptCB_GetOnlineOpts()
	this.OnlinePrefs = prefs
	form.elements.voiceenable.selValue = ifs_opt_general_fnBoolToSelValue(prefs.bVoiceEnable)
	form.elements.voicerecord.selValue = prefs.iVoiceRecordVol
	form.elements.voiceplayback.selValue = prefs.iVoicePlayVol
	form.elements.voicesensitivity.selValue = prefs.iVoiceSensitivity

	local devices = prefs.aVoiceRecordDevices
	if not devices or table.getn(devices) == 0 then
		devices = {{name = "Default Input Device", value = 254}}
	end
	this.fcVoiceDevices = devices
	local values = {}
	local selected = 1
	for index, device in ipairs(devices) do
		values[index] = {label = ScriptCB_tounicode(device.name)}
		if device.value == prefs.iVoiceRecordDevice then
			selected = index
		end
	end
	form.elements.microphone.values = values
	form.elements.microphone.selValue = selected
	Form_SetValues(form)

	RoundIFButtonLabel_fnSetUString(form.buttons.voicerecord,
		ScriptCB_tounicode(string.format("%d%%", prefs.iVoiceRecordVol * 10)))
	RoundIFButtonLabel_fnSetUString(form.buttons.voiceplayback,
		ScriptCB_tounicode(string.format("%d%%", prefs.iVoicePlayVol * 10)))
	RoundIFButtonLabel_fnSetUString(form.buttons.voicesensitivity,
		ScriptCB_tounicode(string.format("%d dB", fc_voice_options_Sensitivity[prefs.iVoiceSensitivity + 1])))
	-- The stored push-to-talk choice has no native input consumer; capture uses voice activity.
	RoundIFButtonLabel_fnSetUString(form.buttons.voiceinputmode, ScriptCB_tounicode("Voice Activity"))
	fc_voice_options_Test(this, this.fcVoiceTest)
	if this.bShellMode then
		IFObj_fnSetVis(this.cancelbutton, ScriptCB_HasProfileChanged())
	end
end

-- Apply the new fields and the stock voice sliders through the normal settings setter.
local function fc_voice_options_Changed(form, element)
	local this = ifs_opt_mp
	if element.tag == "microphonetest" then
		fc_voice_options_Test(this, not this.fcVoiceTest)
		return
	end

	local prefs = ScriptCB_GetOnlineOpts()
	if element.tag == "microphone" then
		prefs.iVoiceRecordDevice = this.fcVoiceDevices[element.selValue].value
	else
		local fields = {
			voicerecord = "iVoiceRecordVol",
			voiceplayback = "iVoicePlayVol",
			voicesensitivity = "iVoiceSensitivity",
		}
		prefs[fields[element.tag]] = math.floor(element.selValue + 0.5)
	end
	ScriptCB_SetOnlineOpts(prefs)
	fc_voice_options_Update(this)
end

-- Correct the stock voice-enable callback without changing other radio settings.
local function fc_voice_options_Radio(group, selection)
	local prefs = ScriptCB_GetOnlineOpts()
	prefs.bVoiceEnable = selection == 2
	ScriptCB_SetOnlineOpts(prefs)
	fc_voice_options_Update(ifs_opt_mp)
end

-- Move through visible controls in display order, or through an open microphone list.
local function fc_voice_options_Move(this, step)
	local form = this.formcontainer.form
	local dropdown = form.dropdowns.microphone
	if dropdown.expanded then
		local navigate = step < 0 and ListManager_fnNavUp or ListManager_fnNavDown
		navigate(dropdown.listbox, dropdown.listbox.Contents, dropdown.listbox.Layout)
		-- PC lists keep mouse and keyboard positions separately; accept the item reached by the keys.
		dropdown.listbox.Layout.CursorIdx = dropdown.listbox.Layout.SelectedIdx
		ListManager_fnMoveCursor(dropdown.listbox, dropdown.listbox.Layout)
		return
	end
	if gCurEditbox then
		return
	end

	local tags = {}
	for _, row in ipairs(form.layout.buttonlist) do
		if not form.elements[row.tag].hidden and row.tag ~= "voiceinputmode" then
			table.insert(tags, row.tag)
		end
	end

	local footer = {_cancel = this.cancelbutton, _reset = this.resetbutton, _ok = this.donebutton}
	for _, tag in ipairs({"_cancel", "_reset", "_ok"}) do
		if IFObj_fnGetVis(footer[tag]) then
			table.insert(tags, tag)
		end
	end

	local current = Form_GetMouseOverTag(form) or this.CurButton
	-- Transfer focus from the last mouse position until the mouse moves again.
	gMouseOverHorz = nil
	gMouseListBox = nil
	local index = step > 0 and 0 or 1
	for i, tag in ipairs(tags) do
		if tag == current then
			index = i
		end
	end

	local tag = tags[math.mod(index - 1 + step + table.getn(tags), table.getn(tags)) + 1]
	SetCurButton(tag, this)
	-- Highlight the row label, including rows whose radio or slider has no ordinary button label.
	gCurHiliteButton = footer[tag] or {label = form.text[tag]}
	IFButton_fnSelect(gCurHiliteButton, 1)
	ScriptCB_SndPlaySound("shell_select_change")
end

-- Step radio choices, voice sliders and microphones for keyboard or controller input.
local function fc_voice_options_Adjust(this, step)
	local form = this.formcontainer.form
	local tag = Form_GetMouseOverTag(form) or this.CurButton or ""
	local element = form.elements[tag]
	if element and element.control == "radio" then
		element.fnChanged(form.radiobuttons[tag], Form_ClampValue(element.selValue + step, 1, table.getn(element.values)))
		return true
	elseif tag == "microphone" then
		element.selValue = Form_ClampValue(element.selValue + step, 1, table.getn(this.fcVoiceDevices))
	elseif tag == "voicerecord" or tag == "voiceplayback" or tag == "voicesensitivity" then
		element.selValue = Form_ClampValue(element.selValue + step, 0, 10)
	else
		return
	end
	fc_voice_options_Changed(form, element)
	return true
end

-- Extend the constructed PC form before AddIFScreen creates native controls.
local function fc_voice_options_Multiplayer(this)
	local form = this.formcontainer.form
	local width, height = ScriptCB_GetSafeScreenInfo()
	local controlWidth = width * 0.35
	local devices = {}
	for index = 1, 6 do
		devices[index] = {label = ScriptCB_tounicode("")}
	end
	fc_voice_options_AddRows(form, {
		{tag = "microphone", title = "", control = "dropdown", values = devices, fnChanged = fc_voice_options_Changed},
		{tag = "microphonetest", title = "", values = {"", ""}, fnChanged = fc_voice_options_Changed},
		{tag = "voiceinputmode", title = "", values = {""}},
		{tag = "voicesensitivity", title = "", control = "slider", minValue = 0, maxValue = 10,
			fnChanged = fc_voice_options_Changed, sliderMultiplier = 1},
	}, controlWidth)

	-- Fit the original network settings and voice rows between the tabs and footer.
	this.formcontainer.ScreenRelativeX = 0.4
	form.layout.yTop = -height * 0.3
	form.layout.yHeight = height * 0.06
	form.layout.ySpacing = 0
	form.text.y = 0
	form.sliders.x = 20
	form.dropdowns.x = 20
	form.buttons.x = controlWidth + 35
	for _, row in ipairs(form.layout.buttonlist) do
		local label = form.text[row.tag]
		label.textw = width * 0.4 - 10
		label.texth = form.layout.yHeight
		label.x = -label.textw
		label.x2 = label.x
	end
	local labels = {
		microphone = "Microphone",
		microphonetest = "Microphone Test",
		voiceinputmode = "Input Mode",
		voicesensitivity = "Input Sensitivity",
		voicerecord = "Microphone Volume",
		voiceplayback = "Speaker Volume",
	}
	for tag, label in pairs(labels) do
		IFText_fnSetUString(form.text[tag], ScriptCB_tounicode(label))
	end
	form.buttons.microphonetest = NewPCIFButton {
		btnw = 150,
		btnh = 25,
		font = form.layout.font,
		tag = "microphonetest",
		string = "",
		noTransitionFlash = 1,
	}

	form.buttons.voiceinputmode.inert_all = 1
	local inputMode = form.buttons.voiceinputmode.label
	inputMode.textw = controlWidth
	inputMode.texth = form.layout.yHeight
	inputMode.x = -controlWidth - 15
	inputMode.y = -form.layout.yHeight * 0.5
	inputMode.halign = "left"

	form.elements.voiceenable.fnChanged = fc_voice_options_Radio
	form.radiobuttons.voiceenable.callback = fc_voice_options_Radio
	for _, tag in ipairs({"voicerecord", "voiceplayback", "voicesensitivity"}) do
		form.elements[tag].maxValue = 10
		form.elements[tag].sliderMultiplier = 10
		form.elements[tag].fnChanged = fc_voice_options_Changed

		-- Recreate the stock narrow sliders so their images and mouse bounds use the same width.
		form.sliders[tag] = NewHSlider {
			x = controlWidth * 0.5,
			width = controlWidth,
			height = 24,
			thumbpercentage = 10 / controlWidth,
			texturebg = "slider_sound",
			texturefg = "slider_fg",
		}
		form.sliders[tag].tag = tag

		-- Match the Sound page's right-aligned value column while retaining the new font and slider layout.
		local value = form.buttons[tag].label
		value.x = width * 0.6 - 60 - form.buttons.x
		value.textw = 60
		value.halign = "right"
	end

	-- Put the microphone above the other voice rows so its menu fits below the button.
	local rows = {}
	for _, row in ipairs(form.layout.buttonlist) do
		if row.tag ~= "voiceenable" and not labels[row.tag] then
			table.insert(rows, row)
		end
	end
	for _, tag in ipairs({"voiceenable", "microphone", "microphonetest", "voicerecord",
		"voiceinputmode", "voicesensitivity", "voiceplayback"}) do
		table.insert(rows, {tag = tag})
	end
	form.layout.buttonlist = rows

	-- Stock callbacks retain ownership of tabs, profile saves, reset and cancellation.
	local updateForm = ifs_opt_mp_fnUpdateForm
	ifs_opt_mp_fnUpdateForm = function(form, ...)
		local results = fc_voice_options_Results(updateForm(form, unpack(arg, 1, arg.n)))
		fc_voice_options_Update(ifs_opt_mp)
		return unpack(results, 1, results.n)
	end
	local enter = this.Enter
	this.Enter = function(screen, ...)
		-- Stock Enter enables microphone playback; suppress its echo calls while the form initializes.
		local echo = ScriptCB_SetVoiceLocalEchoEnable
		ScriptCB_SetVoiceLocalEchoEnable = function() end
		local results = fc_voice_options_Results(enter(screen, unpack(arg, 1, arg.n)))
		ScriptCB_SetVoiceLocalEchoEnable = echo
		local form = screen.formcontainer.form
		for _, tag in ipairs({"voiceenable", "voicerecord", "voiceplayback"}) do
			form.elements[tag].hidden = nil
		end
		form.elements.microphonetest.hidden = not screen.bShellMode
		-- Remaster uses this stock field, but its bandwidth choice has no effect on Galaxy.
		form.elements.players.hidden = true
		Form_ShowHideElements(form)

		-- DoPostDelete discards Lua coordinates; supply x again when stock forms reposition each row.
		for _, row in ipairs(form.layout.buttonlist) do
			local tag = row.tag
			local element = form.elements[tag]
			if element.control == "slider" and not element.hidden then
				IFObj_fnSetPos(form.sliders[tag], form.sliders[tag].width * 0.5, form.buttons[tag].y)
			elseif element.control == "dropdown" and not element.hidden then
				local dropdown = form.dropdowns[tag]
				IFObj_fnSetPos(dropdown.button, dropdown.button.width * 0.5, form.buttons[tag].y)
				IFObj_fnSetPos(dropdown.listbox, dropdown.listbox.width * 0.5,
					form.buttons[tag].y + dropdown.listbox.height * 0.5 + 5)
			end
		end

		IFObj_fnSetPos(form.buttons.microphonetest, -controlWidth * 0.5 - 15, nil)
		fc_voice_options_Test(screen, nil)
		fc_voice_options_Update(screen)
		screen.CurButton = nil
		fc_voice_options_Move(screen, 1)
		return unpack(results, 1, results.n)
	end
	local update = this.Update
	this.Update = function(screen, fDt, ...)
		-- Stock Update subtracts fDt before polling; keep the timer at zero to preserve the PC Voice choice.
		screen.pollProfileTime = fDt
		return update(screen, fDt, unpack(arg, 1, arg.n))
	end
	local exit = this.Exit
	this.Exit = function(screen, ...)
		fc_voice_options_Test(screen, nil)
		Form_CloseDropboxes(screen.formcontainer.form)
		return exit(screen, unpack(arg, 1, arg.n))
	end
	local accept = this.Input_Accept
	this.Input_Accept = function(screen, ...)
		local form = screen.formcontainer.form
		local dropdown = form.dropdowns.microphone
		if dropdown.expanded then
			Form_CloseDropboxes(form)
			if gMouseListBox == dropdown.listbox or screen.CurButton == "microphone" or screen.CurButton == "button_microphone" then
				form.elements.microphone.selValue = dropdown.listbox.Layout.CursorIdx
				fc_voice_options_Changed(form, form.elements.microphone)
			end
			return
		end
		if screen.CurButton == "microphonetest" then
			fc_voice_options_Test(screen, not screen.fcVoiceTest)
			return
		end
		if screen.CurButton == "_reset" then
			fc_voice_options_Test(screen, nil)
		end
		local results = fc_voice_options_Results(accept(screen, unpack(arg, 1, arg.n)))
		fc_voice_options_Update(screen)
		return unpack(results, 1, results.n)
	end
	local back = this.Input_Back
	this.Input_Back = function(screen, ...)
		if Form_CloseDropboxes(screen.formcontainer.form) then
			return
		end
		return back(screen, unpack(arg, 1, arg.n))
	end
	local left = this.Input_GeneralLeft
	this.Input_GeneralLeft = function(screen, ...)
		if fc_voice_options_Adjust(screen, -1) then
			return
		end
		return left(screen, unpack(arg, 1, arg.n))
	end
	local right = this.Input_GeneralRight
	this.Input_GeneralRight = function(screen, ...)
		if fc_voice_options_Adjust(screen, 1) then
			return
		end
		return right(screen, unpack(arg, 1, arg.n))
	end
	this.Input_GeneralUp = function(screen)
		fc_voice_options_Move(screen, -1)
	end
	this.Input_GeneralDown = function(screen)
		fc_voice_options_Move(screen, 1)
	end
end

-- Offer Disabled and Server relay in the host form.
local function fc_voice_options_Instant(this)
	local form = this.screens.host.form
	if not form.elements.voicemode then
		fc_voice_options_AddRows(form, {{
			tag = "voicemode",
			title = "",
			control = "dropdown",
			values = {
				"ifs.mp.voicechat.mode.disabled",
				"ifs.mp.voicechat.mode.serverrelay",
			},
			-- Menu choices 1 and 2 select native modes 1 and 4.
			fnChanged = function(form, element)
				ifs_instant_options.GamePrefs.iVoiceMode = element.selValue == 1 and 1 or 4
			end,
		}}, form.layout.width)
		IFText_fnSetUString(form.text.voicemode, ScriptCB_tounicode("Voice Mode"))
	end
	local found
	for _, tag in ipairs(ifs_io_listtags_host.tags) do
		found = found or tag == "voicemode"
	end
	if not found then
		table.insert(ifs_io_listtags_host.tags, "voicemode")
	end

	-- set_defaults rereads native preferences on Enter; initialize the cached native mode too.
	this.GamePrefs.iVoiceMode = 4
	ScriptCB_SetNetGameDefaults(this.GamePrefs)
	-- Keyboard adjustments use the same two modes as the dropdown.
	local adjust = ifs_instant_options_fnAdjustItem
	ifs_instant_options_fnAdjustItem = function(screen, tag, step, ...)
		if tag == "voicemode" then
			if step ~= 0 then
				screen.GamePrefs.iVoiceMode = step < 0 and 1 or 4
			end
			return
		end
		return adjust(screen, tag, step, unpack(arg, 1, arg.n))
	end

	local realValue = ifs_io_GetRealValueFor
	ifs_io_GetRealValueFor = function(form, tag, ...)
		if tag == "voicemode" then
			form.elements[tag].selValue = ifs_instant_options.GamePrefs.iVoiceMode == 1 and 1 or 2
			return
		end
		return realValue(form, tag, unpack(arg, 1, arg.n))
	end
end

-- Extend the selected screen before AddIFScreen creates its native controls.
function fc_voice_options_Extend(screen, name)
	if screen.fcVoiceOptions or gPlatformStr ~= "PC" then
		return
	end
	if name == "ifs_opt_mp" then
		fc_voice_options_Multiplayer(screen)
	elseif name == "ifs_instant_options" then
		fc_voice_options_Instant(screen)
	else
		return
	end
	screen.fcVoiceOptions = 1
end
