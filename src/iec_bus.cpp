// Pi1541 - A Commodore 1541 disk drive emulator
// Copyright(C) 2018 Stephen White
//
// This file is part of Pi1541.
// 
// Pi1541 is free software : you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// Pi1541 is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with Pi1541. If not, see <http://www.gnu.org/licenses/>.

#include "iec_bus.h"
#include "InputMappings.h"

static int buttonCount = sizeof(ButtonPinFlags) / sizeof(unsigned);

u32 IEC_Bus::PIGPIO_MASK_IN_ATN = 1 << PIGPIO_ATN;
u32 IEC_Bus::PIGPIO_MASK_IN_DATA = 1 << PIGPIO_DATA;
u32 IEC_Bus::PIGPIO_MASK_IN_CLOCK = 1 << PIGPIO_CLOCK;
u32 IEC_Bus::PIGPIO_MASK_IN_SRQ = 1 << PIGPIO_SRQ;
u32 IEC_Bus::PIGPIO_MASK_IN_RESET = 1 << PIGPIO_RESET;

bool IEC_Bus::PI_Atn = false;
bool IEC_Bus::PI_Data = false;
bool IEC_Bus::PI_Clock = false;
bool IEC_Bus::PI_SRQ = false;
bool IEC_Bus::PI_Reset = false;

bool IEC_Bus::VIA_Atna = false;
bool IEC_Bus::VIA_Data = false;
bool IEC_Bus::VIA_Clock = false;

bool IEC_Bus::DataSetToOut = false;
bool IEC_Bus::AtnaDataSetToOut = false;
bool IEC_Bus::ClockSetToOut = false;
bool IEC_Bus::SRQSetToOut = false;

m6522* IEC_Bus::VIA = 0;
m8520* IEC_Bus::CIA = 0;
IOPort* IEC_Bus::port = 0;

bool IEC_Bus::OutputLED = false;
bool IEC_Bus::OutputSound = false;

bool IEC_Bus::Resetting = false;

bool IEC_Bus::splitIECLines = false;
bool IEC_Bus::invertIECInputs = false;
bool IEC_Bus::invertIECOutputs = true;
bool IEC_Bus::ignoreReset = false;

u32 IEC_Bus::myOutsGPFSEL1 = 0;
u32 IEC_Bus::myOutsGPFSEL0 = 0;
bool IEC_Bus::InputButton[5] = { 0 };
bool IEC_Bus::InputButtonPrev[5] = { 0 };
u32 IEC_Bus::validInputCount[5] = { 0 };
u32 IEC_Bus::inputRepeatThreshold[5];
u32 IEC_Bus::inputRepeat[5] = { 0 };
u32 IEC_Bus::inputRepeatPrev[5] = { 0 };


u32 IEC_Bus::emulationModeCheckButtonIndex = 0;
u8  IEC_Bus::upDownRotary = 0;


unsigned IEC_Bus::gplev0;


void IEC_Bus::ReadBrowseMode(void)
{
	IOPort* portB = 0;
	gplev0 = read32(ARM_GPIO_GPLEV0);

	int buttonIndex;
	for (buttonIndex = 0; buttonIndex < buttonCount; ++buttonIndex)
	{
		UpdateButton(buttonIndex, gplev0);
	}

	if (GetInputButtonPressed(1))
	{
		if (InputButton[2])
			IEC_Bus::upDownRotary = DOWN_FLAG;
		else
			IEC_Bus::upDownRotary = UP_FLAG;
	}
	
	bool ATNIn = (gplev0 & PIGPIO_MASK_IN_ATN) == (invertIECInputs ? PIGPIO_MASK_IN_ATN : 0);
	if (PI_Atn != ATNIn)
	{
		PI_Atn = ATNIn;
	}

	if (portB && (portB->GetDirection() & 0x10) == 0)
		AtnaDataSetToOut = false; // If the ATNA PB4 gets set to an input then we can't be pulling data low. (Maniac Mansion does this)

	if (!AtnaDataSetToOut && !DataSetToOut)	// only sense if we have not brought the line low (because we can't as we have the pin set to output but we can simulate in software)
	{
		bool DATAIn = (gplev0 & PIGPIO_MASK_IN_DATA) == (invertIECInputs ? PIGPIO_MASK_IN_DATA : 0);
		if (PI_Data != DATAIn)
		{
			PI_Data = DATAIn;
		}
	}
	else
	{
		PI_Data = true;
	}

	if (!ClockSetToOut)	// only sense if we have not brought the line low (because we can't as we have the pin set to output but we can simulate in software)
	{
		bool CLOCKIn = (gplev0 & PIGPIO_MASK_IN_CLOCK) == (invertIECInputs ? PIGPIO_MASK_IN_CLOCK  : 0);
		if (PI_Clock != CLOCKIn)
		{
			PI_Clock = CLOCKIn;
		}
	}
	else
	{
		PI_Clock = true;
	}

	Resetting = !ignoreReset && ((gplev0 & PIGPIO_MASK_IN_RESET) == (invertIECInputs ? PIGPIO_MASK_IN_RESET : 0));
}

void IEC_Bus::ReadButtonsEmulationMode(void)
{
	int buttonIndex;
	for (buttonIndex = 0; buttonIndex < 3; ++buttonIndex)
	{
		UpdateButton(buttonIndex, gplev0);
	}
	if (GetInputButtonPressed(1))
	{
		if (InputButton[2])
			upDownRotary = NEXT_FLAG;
		else
			upDownRotary = PREV_FLAG;
	}
}

void IEC_Bus::ReadEmulationMode1541(void)
{
	bool AtnaDataSetToOutOld = AtnaDataSetToOut;
	IOPort* portB = 0;
	gplev0 = read32(ARM_GPIO_GPLEV0);

	portB = port;

	bool ATNIn = (gplev0 & PIGPIO_MASK_IN_ATN) == (invertIECInputs ? PIGPIO_MASK_IN_ATN : 0);
	if (PI_Atn != ATNIn)
	{
		PI_Atn = ATNIn;

		//DEBUG_LOG("A%d\r\n", PI_Atn);
		//if (port)
		{
			if ((portB->GetDirection() & 0x10) != 0)
			{
				// Emulate the XOR gate UD3
				// We only need to do this when fully emulating, iec commands do this internally
				AtnaDataSetToOut = (VIA_Atna != PI_Atn);
			}

			portB->SetInput(VIAPORTPINS_ATNIN, ATNIn);	//is inverted and then connected to pb7 and ca1
			VIA->InputCA1(ATNIn);
		}
	}

	if (portB && (portB->GetDirection() & 0x10) == 0)
		AtnaDataSetToOut = false; // If the ATNA PB4 gets set to an input then we can't be pulling data low. (Maniac Mansion does this)

	// moved from PortB_OnPortOut
	if (AtnaDataSetToOut)
		portB->SetInput(VIAPORTPINS_DATAIN, true);	// simulate the read in software

	if (!AtnaDataSetToOut && !DataSetToOut)	// only sense if we have not brought the line low (because we can't as we have the pin set to output but we can simulate in software)
	{
		bool DATAIn = (gplev0 & PIGPIO_MASK_IN_DATA) == (invertIECInputs ? PIGPIO_MASK_IN_DATA : 0);
		//if (PI_Data != DATAIn)
		{
			PI_Data = DATAIn;
			portB->SetInput(VIAPORTPINS_DATAIN, DATAIn);	// VIA DATAin pb0 output from inverted DIN 5 DATA
		}
	}
	else
	{
		PI_Data = true;
		portB->SetInput(VIAPORTPINS_DATAIN, true);	// simulate the read in software
	}

	if (!ClockSetToOut)	// only sense if we have not brought the line low (because we can't as we have the pin set to output but we can simulate in software)
	{
		bool CLOCKIn = (gplev0 & PIGPIO_MASK_IN_CLOCK) == (invertIECInputs ? PIGPIO_MASK_IN_CLOCK : 0);
		//if (PI_Clock != CLOCKIn)
		{
			PI_Clock = CLOCKIn;
			portB->SetInput(VIAPORTPINS_CLOCKIN, CLOCKIn); // VIA CLKin pb2 output from inverted DIN 4 CLK
		}
	}
	else
	{
		PI_Clock = true;
		portB->SetInput(VIAPORTPINS_CLOCKIN, true); // simulate the read in software
	}

	Resetting = !ignoreReset && ((gplev0 & PIGPIO_MASK_IN_RESET) == (invertIECInputs ? PIGPIO_MASK_IN_RESET : 0));
}

void IEC_Bus::ReadEmulationMode1581(void)
{
	IOPort* portB = 0;
	gplev0 = read32(ARM_GPIO_GPLEV0);

	ReadButtonsEmulationMode();

	portB = port;

	bool ATNIn = (gplev0 & PIGPIO_MASK_IN_ATN) == (invertIECInputs ? PIGPIO_MASK_IN_ATN : 0);
	if (PI_Atn != ATNIn)
	{
		PI_Atn = ATNIn;

		//DEBUG_LOG("A%d\r\n", PI_Atn);
		//if (port)
		{
			if ((portB->GetDirection() & 0x10) != 0)
			{
				// Emulate the XOR gate UD3
				// We only need to do this when fully emulating, iec commands do this internally
				AtnaDataSetToOut = (VIA_Atna & PI_Atn);
			}

			portB->SetInput(VIAPORTPINS_ATNIN, ATNIn);	//is inverted and then connected to pb7 and ca1
			CIA->SetPinFLAG(!ATNIn);
		}
	}

	if (portB && (portB->GetDirection() & 0x10) == 0)
		AtnaDataSetToOut = false; // If the ATNA PB4 gets set to an input then we can't be pulling data low. (Maniac Mansion does this)

	if (!AtnaDataSetToOut && !DataSetToOut)	// only sense if we have not brought the line low (because we can't as we have the pin set to output but we can simulate in software)
	{
		bool DATAIn = (gplev0 & PIGPIO_MASK_IN_DATA) == (invertIECInputs ? PIGPIO_MASK_IN_DATA : 0);
		if (PI_Data != DATAIn)
		{
			PI_Data = DATAIn;
			portB->SetInput(VIAPORTPINS_DATAIN, DATAIn);	// VIA DATAin pb0 output from inverted DIN 5 DATA
		}
	}
	else
	{
		PI_Data = true;
		portB->SetInput(VIAPORTPINS_DATAIN, true);	// simulate the read in software
	}

	if (!ClockSetToOut)	// only sense if we have not brought the line low (because we can't as we have the pin set to output but we can simulate in software)
	{
		bool CLOCKIn = (gplev0 & PIGPIO_MASK_IN_CLOCK) == (invertIECInputs ? PIGPIO_MASK_IN_CLOCK : 0);
		if (PI_Clock != CLOCKIn)
		{
			PI_Clock = CLOCKIn;
			portB->SetInput(VIAPORTPINS_CLOCKIN, CLOCKIn); // VIA CLKin pb2 output from inverted DIN 4 CLK
		}
	}
	else
	{
		PI_Clock = true;
		portB->SetInput(VIAPORTPINS_CLOCKIN, true); // simulate the read in software
	}

	if (!SRQSetToOut)	// only sense if we have not brought the line low (because we can't as we have the pin set to output but we can simulate in software)
	{
		bool SRQIn = (gplev0 & PIGPIO_MASK_IN_SRQ) == (invertIECInputs ? PIGPIO_MASK_IN_SRQ : 0);
		if (PI_SRQ != SRQIn)
		{
			PI_SRQ = SRQIn;
		}
	}
	else
	{
		PI_SRQ = true;
	}

	Resetting = !ignoreReset && ((gplev0 & PIGPIO_MASK_IN_RESET) == (invertIECInputs ? PIGPIO_MASK_IN_RESET : 0));
}

void IEC_Bus::RefreshOuts1541(void)
{
	unsigned set = 0;
	unsigned clear = 0;
	unsigned tmp;

	if (!splitIECLines)
	{
		unsigned outputs = 0;

		if (AtnaDataSetToOut || DataSetToOut) outputs |= (FS_OUTPUT << ((PIGPIO_DATA - 10) * 3));
		if (ClockSetToOut) outputs |= (FS_OUTPUT << ((PIGPIO_CLOCK - 10) * 3));

		unsigned nValue = (myOutsGPFSEL1 & PI_OUTPUT_MASK_GPFSEL1) | outputs;
		write32(ARM_GPIO_GPFSEL1, nValue);
	}
	else
	{
		if (AtnaDataSetToOut || DataSetToOut) set |= 1 << PIGPIO_OUT_DATA;
		else clear |= 1 << PIGPIO_OUT_DATA;

		if (ClockSetToOut) set |= 1 << PIGPIO_OUT_CLOCK;
		else clear |= 1 << PIGPIO_OUT_CLOCK;

		if (!invertIECOutputs) {
			tmp = set;
			set = clear;
			clear = tmp;
		}
	}

	if (OutputLED) set |= 1 << PIGPIO_OUT_LED;
	else clear |= 1 << PIGPIO_OUT_LED;

	if (OutputSound) set |= 1 << PIGPIO_OUT_SOUND;
	else clear |= 1 << PIGPIO_OUT_SOUND;

	write32(ARM_GPIO_GPCLR0, clear);
	write32(ARM_GPIO_GPSET0, set);
}

void IEC_Bus::PortB_OnPortOut(void* pUserData, unsigned char status)
{
	bool oldDataSetToOut = DataSetToOut;
	bool oldClockSetToOut = ClockSetToOut;
	bool AtnaDataSetToOutOld = AtnaDataSetToOut;

	// These are the values the VIA is trying to set the outputs to
	VIA_Atna = (status & (unsigned char)VIAPORTPINS_ATNAOUT) != 0;
	VIA_Data = (status & (unsigned char)VIAPORTPINS_DATAOUT) != 0;		// VIA DATAout PB1 inverted and then connected to DIN DATA
	VIA_Clock = (status & (unsigned char)VIAPORTPINS_CLOCKOUT) != 0;	// VIA CLKout PB3 inverted and then connected to DIN CLK

	if (VIA)
	{
		// Emulate the XOR gate UD3
		AtnaDataSetToOut = (VIA_Atna != PI_Atn);
	}
	else
	{
		AtnaDataSetToOut = (VIA_Atna & PI_Atn);
	}

	//if (AtnaDataSetToOut)
	//{
	//	// if the output of the XOR gate is high (ie VIA_Atna != PI_Atn) then this is inverted and pulls DATA low (activating it)
	//	//PI_Data = true;
	//	if (port) port->SetInput(VIAPORTPINS_DATAIN, true);	// simulate the read in software
	//}

	if (VIA && port)
	{
		// If the VIA's data and clock outputs ever get set to inputs the real hardware reads these lines as asserted.
		bool PB1SetToInput = (port->GetDirection() & 2) == 0;
		bool PB3SetToInput = (port->GetDirection() & 8) == 0;
		if (PB1SetToInput) VIA_Data = true;
		if (PB3SetToInput) VIA_Clock = true;
	}

	ClockSetToOut = VIA_Clock;
	DataSetToOut = VIA_Data;

	//if (!oldDataSetToOut && DataSetToOut)
	//{
	//	//PI_Data = true;
	//	if (port) port->SetInput(VIAPORTPINS_DATAOUT, true); // simulate the read in software
	//}

	//if (!oldClockSetToOut && ClockSetToOut)
	//{
	//	//PI_Clock = true;
	//	if (port) port->SetInput(VIAPORTPINS_CLOCKIN, true); // simulate the read in software
	//}

	//if (AtnaDataSetToOutOld ^ AtnaDataSetToOut)
	//	RefreshOuts1541();
}
