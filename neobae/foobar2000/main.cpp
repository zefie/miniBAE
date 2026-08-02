#include "pch.h"
#include "Version.h"

DECLARE_COMPONENT_VERSION(
	"NeoBAE",
	FOO_NEOBAE_VERSION_STRING,
	"NeoBAE MIDI / RMF / XMF / RMI input for foobar2000.\n"
	"Copyright (C) 2021-2026 Zefie Networks.\n"
	"Based on Beatnik BAE / miniBAE."
);

VALIDATE_COMPONENT_FILENAME("foo_neobae.dll");

FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;
