#include "pch.h"
#include "Configuration.h"
#include "NeoBAEFeatures.h"
#include "Version.h"
#include "resource.h"

#include "NeoBAE.h"

class CNeoBAEPreferences : public CDialogImpl<CNeoBAEPreferences>, public preferences_page_instance {
public:
	CNeoBAEPreferences(preferences_page_callback::ptr callback) : m_callback(callback) {}

	enum { IDD = IDD_NEOBAE_PREFERENCES };

	t_uint32 get_state() override;
	void apply() override;
	void reset() override;

	BEGIN_MSG_MAP_EX(CNeoBAEPreferences)
		MSG_WM_INITDIALOG(OnInitDialog)
		COMMAND_HANDLER_EX(IDC_LOOP, BN_CLICKED, OnChanged)
		COMMAND_HANDLER_EX(IDC_LOOP_INFINITE, BN_CLICKED, OnChanged)
		COMMAND_HANDLER_EX(IDC_USE_BUILTIN_BANK, BN_CLICKED, OnChanged)
		COMMAND_HANDLER_EX(IDC_DLS_COMPAT, BN_CLICKED, OnChanged)
		COMMAND_HANDLER_EX(IDC_NORMALIZE, BN_CLICKED, OnChanged)
		COMMAND_HANDLER_EX(IDC_LOOP_COUNT, EN_CHANGE, OnChanged)
		COMMAND_HANDLER_EX(IDC_BANK_PATH, EN_CHANGE, OnChanged)
		COMMAND_HANDLER_EX(IDC_BANK_BROWSE, BN_CLICKED, OnBrowseBank)
		COMMAND_HANDLER_EX(IDC_SAMPLE_RATE, CBN_SELCHANGE, OnChanged)
	END_MSG_MAP()

private:
	BOOL OnInitDialog(CWindow, LPARAM);
	void OnChanged(UINT, int, CWindow);
	void OnBrowseBank(UINT, int, CWindow);
	bool HasChanged();
	void NotifyHost();
	void UpdateControlEnablement();

	void LoadToUI();
	void SaveFromUI();
	void PopulateSampleRates();
	unsigned GetSelectedSampleRate() const;
	void SelectSampleRate(unsigned hz);

	preferences_page_callback::ptr m_callback;
	fb2k::CDarkModeHooks m_dark;
};

void CNeoBAEPreferences::PopulateSampleRates()
{
	CComboBox box = (CComboBox)GetDlgItem(IDC_SAMPLE_RATE);
	box.ResetContent();
	for (size_t i = 0; i < kNeoBAESampleRateCount; ++i) {
		wchar_t label[32];
		_snwprintf_s(label, _TRUNCATE, L"%u Hz", kNeoBAESampleRates[i]);
		const int idx = box.AddString(label);
		box.SetItemData(idx, kNeoBAESampleRates[i]);
	}
}

unsigned CNeoBAEPreferences::GetSelectedSampleRate() const
{
	CComboBox box = (CComboBox)GetDlgItem(IDC_SAMPLE_RATE);
	const int idx = box.GetCurSel();
	if (idx < 0)
		return (unsigned)kDefaultSampleRate;
	return (unsigned)box.GetItemData(idx);
}

void CNeoBAEPreferences::SelectSampleRate(unsigned hz)
{
	const unsigned normalized = NormalizeSampleRateHz((int64_t)hz);
	CComboBox box = (CComboBox)GetDlgItem(IDC_SAMPLE_RATE);
	for (int i = 0; i < box.GetCount(); ++i) {
		if ((unsigned)box.GetItemData(i) == normalized) {
			box.SetCurSel(i);
			return;
		}
	}
	box.SetCurSel(0);
}

BOOL CNeoBAEPreferences::OnInitDialog(CWindow, LPARAM)
{
	m_dark.AddDialogWithControls(*this);
	PopulateSampleRates();
	LoadToUI();
	UpdateControlEnablement();

	{
		uSetDlgItemText(*this, IDC_PLUGIN_VERSION, FOO_NEOBAE_VERSION_STRING);

		const char* ver = BAE_GetVersion();
		const char* comp = BAE_GetCompileInfo();
		pfc::string8 line;
		line << (ver && ver[0] ? ver : "?");
		if (comp && comp[0])
			line << " (" << comp << ")";
		uSetDlgItemText(*this, IDC_LIB_VERSION, line);

		const char* feat = BAE_GetFeatureString();
		uSetDlgItemText(*this, IDC_LIB_FEATURES, (feat && feat[0]) ? feat : "");

		if (ver)
			free((void*)ver);
		if (comp)
			free((void*)comp);
	}

	return FALSE;
}

void CNeoBAEPreferences::LoadToUI()
{
	CheckDlgButton(IDC_LOOP, cfg::LoopEnabled.get() ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_LOOP_INFINITE, cfg::LoopInfinite.get() ? BST_CHECKED : BST_UNCHECKED);
	SetDlgItemInt(IDC_LOOP_COUNT, (UINT)pfc::max_t<int64_t>(1, cfg::LoopCount.get()), FALSE);

	CheckDlgButton(IDC_USE_BUILTIN_BANK, cfg::UseBuiltinBank.get() ? BST_CHECKED : BST_UNCHECKED);
	uSetDlgItemText(*this, IDC_BANK_PATH, cfg::BankPath.get());

	CheckDlgButton(IDC_DLS_COMPAT, cfg::DLSCompatibilityMode.get() ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_NORMALIZE, cfg::Normalize.get() ? BST_CHECKED : BST_UNCHECKED);
	SelectSampleRate(NormalizeSampleRateHz(cfg::SampleRate.get()));
}

void CNeoBAEPreferences::SaveFromUI()
{
	cfg::LoopEnabled = IsDlgButtonChecked(IDC_LOOP) == BST_CHECKED;
	cfg::LoopInfinite = IsDlgButtonChecked(IDC_LOOP_INFINITE) == BST_CHECKED;

	UINT count = GetDlgItemInt(IDC_LOOP_COUNT, nullptr, FALSE);
	if (count < 1)
		count = 1;
	if (count > 30000)
		count = 30000;
	cfg::LoopCount = (int64_t)count;

	cfg::UseBuiltinBank = IsDlgButtonChecked(IDC_USE_BUILTIN_BANK) == BST_CHECKED;
	{
		pfc::string8 path;
		uGetDlgItemText(*this, IDC_BANK_PATH, path);
		cfg::BankPath = path.get_ptr();
	}

	cfg::DLSCompatibilityMode = IsDlgButtonChecked(IDC_DLS_COMPAT) == BST_CHECKED;
	cfg::Normalize = IsDlgButtonChecked(IDC_NORMALIZE) == BST_CHECKED;
	cfg::SampleRate = (int64_t)GetSelectedSampleRate();
}

void CNeoBAEPreferences::UpdateControlEnablement()
{
	const bool loopOn = IsDlgButtonChecked(IDC_LOOP) == BST_CHECKED;
	const bool infinite = IsDlgButtonChecked(IDC_LOOP_INFINITE) == BST_CHECKED;
	const bool builtin = IsDlgButtonChecked(IDC_USE_BUILTIN_BANK) == BST_CHECKED;

	GetDlgItem(IDC_LOOP_INFINITE).EnableWindow(loopOn);
	GetDlgItem(IDC_LOOP_COUNT).EnableWindow(loopOn && !infinite);
	GetDlgItem(IDC_LOOP_COUNT_LABEL).EnableWindow(loopOn && !infinite);

	GetDlgItem(IDC_BANK_PATH).EnableWindow(!builtin);
	GetDlgItem(IDC_BANK_BROWSE).EnableWindow(!builtin);
	GetDlgItem(IDC_BANK_LABEL).EnableWindow(!builtin);
}

void CNeoBAEPreferences::OnChanged(UINT, int, CWindow)
{
	UpdateControlEnablement();
	NotifyHost();
}

void CNeoBAEPreferences::OnBrowseBank(UINT, int, CWindow)
{
	pfc::string8 path;
	uGetDlgItemText(*this, IDC_BANK_PATH, path);

	pfc::string8 dir;
	if (path.get_length()) {
		const char* slash = strrchr(path.get_ptr(), '\\');
		if (!slash)
			slash = strrchr(path.get_ptr(), '/');
		if (slash)
			dir.set_string(path.get_ptr(), slash - path.get_ptr());
	}

	pfc::string8 selected = path;
	const BOOL ok = uGetOpenFileName(
		m_hWnd,
		"Sound banks|*.hsb;*.zsb;*.sf2;*.sf3;*.sfo;*.dls|HSB / ZSB|*.hsb;*.zsb|SoundFont|*.sf2;*.sf3;*.sfo|DLS|*.dls|All files|*.*",
		0,
		"hsb",
		"Select NeoBAE bank",
		dir.get_length() ? dir.get_ptr() : nullptr,
		selected,
		FALSE);
	if (ok) {
		uSetDlgItemText(*this, IDC_BANK_PATH, selected);
		NotifyHost();
	}
}

t_uint32 CNeoBAEPreferences::get_state()
{
	t_uint32 state = preferences_state::resettable | preferences_state::dark_mode_supported;
	if (HasChanged())
		state |= preferences_state::changed;
	return state;
}

void CNeoBAEPreferences::reset()
{
	CheckDlgButton(IDC_LOOP, kDefaultLoopEnabled ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_LOOP_INFINITE, kDefaultLoopInfinite ? BST_CHECKED : BST_UNCHECKED);
	SetDlgItemInt(IDC_LOOP_COUNT, (UINT)kDefaultLoopCount, FALSE);
	CheckDlgButton(IDC_USE_BUILTIN_BANK, kDefaultUseBuiltinBank ? BST_CHECKED : BST_UNCHECKED);
	uSetDlgItemText(*this, IDC_BANK_PATH, "");
	CheckDlgButton(IDC_DLS_COMPAT, kDefaultDLSCompatibilityMode ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_NORMALIZE, kDefaultNormalize ? BST_CHECKED : BST_UNCHECKED);
	SelectSampleRate((unsigned)kDefaultSampleRate);
	UpdateControlEnablement();
	NotifyHost();
}

void CNeoBAEPreferences::apply()
{
	SaveFromUI();
	NotifyHost();
}

bool CNeoBAEPreferences::HasChanged()
{
	if ((IsDlgButtonChecked(IDC_LOOP) == BST_CHECKED) != cfg::LoopEnabled.get())
		return true;
	if ((IsDlgButtonChecked(IDC_LOOP_INFINITE) == BST_CHECKED) != cfg::LoopInfinite.get())
		return true;

	const UINT count = GetDlgItemInt(IDC_LOOP_COUNT, nullptr, FALSE);
	if ((int64_t)count != cfg::LoopCount.get())
		return true;

	if ((IsDlgButtonChecked(IDC_USE_BUILTIN_BANK) == BST_CHECKED) != cfg::UseBuiltinBank.get())
		return true;

	pfc::string8 path;
	uGetDlgItemText(*this, IDC_BANK_PATH, path);
	if (path != cfg::BankPath.get())
		return true;

	if ((IsDlgButtonChecked(IDC_DLS_COMPAT) == BST_CHECKED) != cfg::DLSCompatibilityMode.get())
		return true;
	if ((IsDlgButtonChecked(IDC_NORMALIZE) == BST_CHECKED) != cfg::Normalize.get())
		return true;

	if (GetSelectedSampleRate() != NormalizeSampleRateHz(cfg::SampleRate.get()))
		return true;

	return false;
}

void CNeoBAEPreferences::NotifyHost()
{
	m_callback->on_state_changed();
}

class preferences_page_neobae : public preferences_page_impl<CNeoBAEPreferences> {
public:
	const char* get_name() override { return "NeoBAE"; }
	GUID get_guid() override { return PreferencesPageGUID; }
	GUID get_parent_guid() override { return preferences_page::guid_input; }
};

static preferences_page_factory_t<preferences_page_neobae> g_preferences_page_neobae_factory;
