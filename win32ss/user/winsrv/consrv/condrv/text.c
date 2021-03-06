/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS Console Driver DLL
 * FILE:            win32ss/user/winsrv/consrv/condrv/text.c
 * PURPOSE:         Console Output Functions for text-mode screen-buffers
 * PROGRAMMERS:     Jeffrey Morlan
 *                  Hermes Belusca-Maito (hermes.belusca@sfr.fr)
 */

/* INCLUDES *******************************************************************/

#include <consrv.h>

#define NDEBUG
#include <debug.h>

#define COMMON_LEAD_TRAIL (COMMON_LVB_LEADING_BYTE | COMMON_LVB_TRAILING_BYTE)

/* GLOBALS ********************************************************************/

/*
 * From MSDN:
 * "The lpMultiByteStr and lpWideCharStr pointers must not be the same.
 *  If they are the same, the function fails, and GetLastError returns
 *  ERROR_INVALID_PARAMETER."
 */
#define ConsoleOutputUnicodeToAnsiChar(Console, dChar, sWChar) \
do { \
    ASSERT((ULONG_PTR)(dChar) != (ULONG_PTR)(sWChar)); \
    WideCharToMultiByte((Console)->OutputCodePage, 0, (sWChar), 1, (dChar), 1, NULL, NULL); \
} while (0)

#define ConsoleOutputAnsiToUnicodeChar(Console, dWChar, sChar) \
do { \
    ASSERT((ULONG_PTR)(dWChar) != (ULONG_PTR)(sChar)); \
    MultiByteToWideChar((Console)->OutputCodePage, 0, (sChar), 1, (dWChar), 1); \
} while (0)

/* PRIVATE FUNCTIONS **********************************************************/

CONSOLE_IO_OBJECT_TYPE
TEXTMODE_BUFFER_GetType(PCONSOLE_SCREEN_BUFFER This)
{
    // return This->Header.Type;
    return TEXTMODE_BUFFER;
}

static CONSOLE_SCREEN_BUFFER_VTBL TextVtbl =
{
    TEXTMODE_BUFFER_GetType,
};


/*static*/ VOID
ClearLineBuffer(PTEXTMODE_SCREEN_BUFFER Buff);


NTSTATUS
CONSOLE_SCREEN_BUFFER_Initialize(OUT PCONSOLE_SCREEN_BUFFER* Buffer,
                                 IN PCONSOLE Console,
                                 IN PCONSOLE_SCREEN_BUFFER_VTBL Vtbl,
                                 IN SIZE_T Size);
VOID
CONSOLE_SCREEN_BUFFER_Destroy(IN OUT PCONSOLE_SCREEN_BUFFER Buffer);


NTSTATUS
TEXTMODE_BUFFER_Initialize(OUT PCONSOLE_SCREEN_BUFFER* Buffer,
                           IN PCONSOLE Console,
                           IN HANDLE ProcessHandle,
                           IN PTEXTMODE_BUFFER_INFO TextModeInfo)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PTEXTMODE_SCREEN_BUFFER NewBuffer = NULL;

    UNREFERENCED_PARAMETER(ProcessHandle);

    if (Console == NULL || Buffer == NULL || TextModeInfo == NULL)
        return STATUS_INVALID_PARAMETER;

    *Buffer = NULL;

    Status = CONSOLE_SCREEN_BUFFER_Initialize((PCONSOLE_SCREEN_BUFFER*)&NewBuffer,
                                              Console,
                                              &TextVtbl,
                                              sizeof(TEXTMODE_SCREEN_BUFFER));
    if (!NT_SUCCESS(Status)) return Status;
    NewBuffer->Header.Type = TEXTMODE_BUFFER;

    NewBuffer->Buffer = ConsoleAllocHeap(HEAP_ZERO_MEMORY,
                                         TextModeInfo->ScreenBufferSize.X *
                                         TextModeInfo->ScreenBufferSize.Y *
                                            sizeof(CHAR_INFO));
    if (NewBuffer->Buffer == NULL)
    {
        CONSOLE_SCREEN_BUFFER_Destroy((PCONSOLE_SCREEN_BUFFER)NewBuffer);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    NewBuffer->ScreenBufferSize = NewBuffer->OldScreenBufferSize
                                = TextModeInfo->ScreenBufferSize;
    NewBuffer->ViewSize = NewBuffer->OldViewSize
                        = Console->ConsoleSize;

    NewBuffer->ViewOrigin.X = NewBuffer->ViewOrigin.Y = 0;
    NewBuffer->VirtualY = 0;

    NewBuffer->CursorBlinkOn = NewBuffer->ForceCursorOff = FALSE;
    NewBuffer->CursorInfo.bVisible = (TextModeInfo->IsCursorVisible && (TextModeInfo->CursorSize != 0));
    NewBuffer->CursorInfo.dwSize   = min(max(TextModeInfo->CursorSize, 0), 100);

    NewBuffer->ScreenDefaultAttrib = TextModeInfo->ScreenAttrib;
    NewBuffer->PopupDefaultAttrib  = TextModeInfo->PopupAttrib;

    /* Initialize buffer to be empty with default attributes */
    for (NewBuffer->CursorPosition.Y = 0 ; NewBuffer->CursorPosition.Y < NewBuffer->ScreenBufferSize.Y; NewBuffer->CursorPosition.Y++)
    {
        ClearLineBuffer(NewBuffer);
    }
    NewBuffer->CursorPosition.X = NewBuffer->CursorPosition.Y = 0;

    NewBuffer->Mode = ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT;

    *Buffer = (PCONSOLE_SCREEN_BUFFER)NewBuffer;
    return STATUS_SUCCESS;
}

VOID
TEXTMODE_BUFFER_Destroy(IN OUT PCONSOLE_SCREEN_BUFFER Buffer)
{
    PTEXTMODE_SCREEN_BUFFER Buff = (PTEXTMODE_SCREEN_BUFFER)Buffer;

    /*
     * IMPORTANT !! Reinitialize the type so that we don't enter a recursive
     * infinite loop when calling CONSOLE_SCREEN_BUFFER_Destroy.
     */
    Buffer->Header.Type = SCREEN_BUFFER;

    ConsoleFreeHeap(Buff->Buffer);

    CONSOLE_SCREEN_BUFFER_Destroy(Buffer);
}


PCHAR_INFO
ConioCoordToPointer(PTEXTMODE_SCREEN_BUFFER Buff, ULONG X, ULONG Y)
{
    return &Buff->Buffer[((Y + Buff->VirtualY) % Buff->ScreenBufferSize.Y) * Buff->ScreenBufferSize.X + X];
}

/*static*/ VOID
ClearLineBuffer(PTEXTMODE_SCREEN_BUFFER Buff)
{
    PCHAR_INFO Ptr = ConioCoordToPointer(Buff, 0, Buff->CursorPosition.Y);
    SHORT Pos;

    for (Pos = 0; Pos < Buff->ScreenBufferSize.X; Pos++, Ptr++)
    {
        /* Fill the cell */
        Ptr->Char.UnicodeChar = L' ';
        Ptr->Attributes = Buff->ScreenDefaultAttrib;
    }
}

static VOID
ConioComputeUpdateRect(IN PTEXTMODE_SCREEN_BUFFER Buff,
                       IN OUT PSMALL_RECT UpdateRect,
                       IN PCOORD Start,
                       IN UINT Length)
{
    if ((UINT)Buff->ScreenBufferSize.X <= Start->X + Length)
    {
        UpdateRect->Left  = 0;
        UpdateRect->Right = Buff->ScreenBufferSize.X - 1;
    }
    else
    {
        UpdateRect->Left  = Start->X;
        UpdateRect->Right = Start->X + Length - 1;
    }
    UpdateRect->Top = Start->Y;
    UpdateRect->Bottom = Start->Y + (Start->X + Length - 1) / Buff->ScreenBufferSize.X;
    if (Buff->ScreenBufferSize.Y <= UpdateRect->Bottom)
    {
        UpdateRect->Bottom = Buff->ScreenBufferSize.Y - 1;
    }
}

/*
 * Move from one rectangle to another. We must be careful about the order that
 * this is done, to avoid overwriting parts of the source before they are moved.
 */
static VOID
ConioMoveRegion(PTEXTMODE_SCREEN_BUFFER ScreenBuffer,
                PSMALL_RECT SrcRegion,
                PSMALL_RECT DstRegion,
                PSMALL_RECT ClipRegion,
                CHAR_INFO FillChar)
{
    int Width  = ConioRectWidth(SrcRegion);
    int Height = ConioRectHeight(SrcRegion);
    int SX, SY;
    int DX, DY;
    int XDelta, YDelta;
    int i, j;

    SY = SrcRegion->Top;
    DY = DstRegion->Top;
    YDelta = 1;
    if (SY < DY)
    {
        /* Moving down: work from bottom up */
        SY = SrcRegion->Bottom;
        DY = DstRegion->Bottom;
        YDelta = -1;
    }
    for (i = 0; i < Height; i++)
    {
        PCHAR_INFO SRow = ConioCoordToPointer(ScreenBuffer, 0, SY);
        PCHAR_INFO DRow = ConioCoordToPointer(ScreenBuffer, 0, DY);

        SX = SrcRegion->Left;
        DX = DstRegion->Left;
        XDelta = 1;
        if (SX < DX)
        {
            /* Moving right: work from right to left */
            SX = SrcRegion->Right;
            DX = DstRegion->Right;
            XDelta = -1;
        }
        for (j = 0; j < Width; j++)
        {
            CHAR_INFO Cell = SRow[SX];
            if (SX >= ClipRegion->Left && SX <= ClipRegion->Right &&
                SY >= ClipRegion->Top  && SY <= ClipRegion->Bottom)
            {
                SRow[SX] = FillChar;
            }
            if (DX >= ClipRegion->Left && DX <= ClipRegion->Right &&
                DY >= ClipRegion->Top  && DY <= ClipRegion->Bottom)
            {
                DRow[DX] = Cell;
            }
            SX += XDelta;
            DX += XDelta;
        }
        SY += YDelta;
        DY += YDelta;
    }
}

// FIXME!
NTSTATUS NTAPI
ConDrvWriteConsoleInput(IN PCONSOLE Console,
                        IN PCONSOLE_INPUT_BUFFER InputBuffer,
                        IN BOOLEAN AppendToEnd,
                        IN PINPUT_RECORD InputRecord,
                        IN ULONG NumEventsToWrite,
                        OUT PULONG NumEventsWritten OPTIONAL);

NTSTATUS
ConioResizeBuffer(PCONSOLE Console,
                  PTEXTMODE_SCREEN_BUFFER ScreenBuffer,
                  COORD Size)
{
    PCHAR_INFO Buffer;
    DWORD Offset = 0;
    PCHAR_INFO ptr;
    WORD CurrentAttribute;
    USHORT CurrentY;
    PCHAR_INFO OldBuffer;
    DWORD i;
    DWORD diff;

    /* Buffer size is not allowed to be smaller than the view size */
    if (Size.X < ScreenBuffer->ViewSize.X || Size.Y < ScreenBuffer->ViewSize.Y)
        return STATUS_INVALID_PARAMETER;

    if (Size.X == ScreenBuffer->ScreenBufferSize.X && Size.Y == ScreenBuffer->ScreenBufferSize.Y)
    {
        // FIXME: Trigger a buffer resize event ??
        return STATUS_SUCCESS;
    }

    if (Console->FixedSize)
    {
        /*
         * The console is in fixed-size mode, so we cannot resize anything
         * at the moment. However, keep those settings somewhere so that
         * we can try to set them up when we will be allowed to do so.
         */
        ScreenBuffer->OldScreenBufferSize = Size;
        return STATUS_NOT_SUPPORTED; // STATUS_SUCCESS
    }

    Buffer = ConsoleAllocHeap(HEAP_ZERO_MEMORY, Size.X * Size.Y * sizeof(CHAR_INFO));
    if (!Buffer) return STATUS_NO_MEMORY;

    DPRINT("Resizing (%d,%d) to (%d,%d)\n", ScreenBuffer->ScreenBufferSize.X, ScreenBuffer->ScreenBufferSize.Y, Size.X, Size.Y);
    OldBuffer = ScreenBuffer->Buffer;

    for (CurrentY = 0; CurrentY < ScreenBuffer->ScreenBufferSize.Y && CurrentY < Size.Y; CurrentY++)
    {
        ptr = ConioCoordToPointer(ScreenBuffer, 0, CurrentY);
        if (Size.X <= ScreenBuffer->ScreenBufferSize.X)
        {
            /* Reduce size */
            RtlCopyMemory(Buffer + Offset, ptr, Size.X * sizeof(CHAR_INFO));
            Offset += Size.X;
        }
        else
        {
            /* Enlarge size */
            RtlCopyMemory(Buffer + Offset, ptr, ScreenBuffer->ScreenBufferSize.X * sizeof(CHAR_INFO));
            Offset += ScreenBuffer->ScreenBufferSize.X;

            /* The attribute to be used is the one of the last cell of the current line */
            CurrentAttribute = ConioCoordToPointer(ScreenBuffer,
                                                   ScreenBuffer->ScreenBufferSize.X - 1,
                                                   CurrentY)->Attributes;

            diff = Size.X - ScreenBuffer->ScreenBufferSize.X;

            /* Zero-out the new part of the buffer */
            for (i = 0; i < diff; i++)
            {
                ptr = Buffer + Offset;
                ptr->Char.UnicodeChar = L' ';
                ptr->Attributes = CurrentAttribute;
                ++Offset;
            }
        }
    }

    if (Size.Y > ScreenBuffer->ScreenBufferSize.Y)
    {
        diff = Size.X * (Size.Y - ScreenBuffer->ScreenBufferSize.Y);

        /* Zero-out the new part of the buffer */
        for (i = 0; i < diff; i++)
        {
            ptr = Buffer + Offset;
            ptr->Char.UnicodeChar = L' ';
            ptr->Attributes = ScreenBuffer->ScreenDefaultAttrib;
            ++Offset;
        }
    }

    (void)InterlockedExchangePointer((PVOID volatile*)&ScreenBuffer->Buffer, Buffer);
    ConsoleFreeHeap(OldBuffer);
    ScreenBuffer->ScreenBufferSize = ScreenBuffer->OldScreenBufferSize = Size;
    ScreenBuffer->VirtualY = 0;

    /* Ensure cursor and window are within buffer */
    if (ScreenBuffer->CursorPosition.X >= Size.X)
        ScreenBuffer->CursorPosition.X = Size.X - 1;
    if (ScreenBuffer->CursorPosition.Y >= Size.Y)
        ScreenBuffer->CursorPosition.Y = Size.Y - 1;
    if (ScreenBuffer->ViewOrigin.X > Size.X - ScreenBuffer->ViewSize.X)
        ScreenBuffer->ViewOrigin.X = Size.X - ScreenBuffer->ViewSize.X;
    if (ScreenBuffer->ViewOrigin.Y > Size.Y - ScreenBuffer->ViewSize.Y)
        ScreenBuffer->ViewOrigin.Y = Size.Y - ScreenBuffer->ViewSize.Y;

    /*
     * Trigger a buffer resize event
     */
    if (Console->InputBuffer.Mode & ENABLE_WINDOW_INPUT)
    {
        ULONG NumEventsWritten;
        INPUT_RECORD er;

        er.EventType = WINDOW_BUFFER_SIZE_EVENT;
        er.Event.WindowBufferSizeEvent.dwSize = ScreenBuffer->ScreenBufferSize;

        // ConioProcessInputEvent(Console, &er);
        ConDrvWriteConsoleInput(Console,
                                &Console->InputBuffer,
                                TRUE,
                                &er,
                                1,
                                &NumEventsWritten);
    }

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
ConDrvChangeScreenBufferAttributes(IN PCONSOLE Console,
                                   IN PTEXTMODE_SCREEN_BUFFER Buffer,
                                   IN USHORT NewScreenAttrib,
                                   IN USHORT NewPopupAttrib)
{
    ULONG X, Y, Length;
    PCHAR_INFO Ptr;

    COORD  TopLeft = {0};
    ULONG  NumCodesToWrite;
    USHORT OldScreenAttrib, OldPopupAttrib;

    if (Console == NULL || Buffer == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Validity check */
    ASSERT(Console == Buffer->Header.Console);

    NumCodesToWrite = Buffer->ScreenBufferSize.X * Buffer->ScreenBufferSize.Y;
    OldScreenAttrib = Buffer->ScreenDefaultAttrib;
    OldPopupAttrib  = Buffer->PopupDefaultAttrib;

    X = TopLeft.X;
    Y = (TopLeft.Y + Buffer->VirtualY) % Buffer->ScreenBufferSize.Y;
    Length = NumCodesToWrite;

    while (Length--)
    {
        Ptr = ConioCoordToPointer(Buffer, X, Y);

        /*
         * Change the current colors only if they are the old ones.
         */

        /* Foreground color */
        if ((Ptr->Attributes & 0x0F) == (OldScreenAttrib & 0x0F))
            Ptr->Attributes = (Ptr->Attributes & 0xFFF0) | (NewScreenAttrib & 0x0F);
        if ((Ptr->Attributes & 0x0F) == (OldPopupAttrib & 0x0F))
            Ptr->Attributes = (Ptr->Attributes & 0xFFF0) | (NewPopupAttrib & 0x0F);

        /* Background color */
        if ((Ptr->Attributes & 0xF0) == (OldScreenAttrib & 0xF0))
            Ptr->Attributes = (Ptr->Attributes & 0xFF0F) | (NewScreenAttrib & 0xF0);
        if ((Ptr->Attributes & 0xF0) == (OldPopupAttrib & 0xF0))
            Ptr->Attributes = (Ptr->Attributes & 0xFF0F) | (NewPopupAttrib & 0xF0);

        // ++Ptr;

        if (++X == Buffer->ScreenBufferSize.X)
        {
            X = 0;

            if (++Y == Buffer->ScreenBufferSize.Y)
            {
                Y = 0;
            }
        }
    }

    /* Save foreground and background colors for both screen and popup */
    Buffer->ScreenDefaultAttrib = (NewScreenAttrib & 0x00FF);
    Buffer->PopupDefaultAttrib  = (NewPopupAttrib  & 0x00FF);

    /* Refresh the display if needed */
    if ((PCONSOLE_SCREEN_BUFFER)Buffer == Console->ActiveBuffer)
    {
        SMALL_RECT UpdateRect;
        ConioComputeUpdateRect(Buffer, &UpdateRect, &TopLeft, NumCodesToWrite);
        TermDrawRegion(Console, &UpdateRect);
    }

    return STATUS_SUCCESS;
}


/* PUBLIC DRIVER APIS *********************************************************/

NTSTATUS NTAPI
ConDrvReadConsoleOutput(IN PCONSOLE Console,
                        IN PTEXTMODE_SCREEN_BUFFER Buffer,
                        IN BOOLEAN Unicode,
                        OUT PCHAR_INFO CharInfo/*Buffer*/,
                        IN OUT PSMALL_RECT ReadRegion)
{
    SHORT X, Y;
    SMALL_RECT ScreenBuffer;
    PCHAR_INFO CurCharInfo;
    SMALL_RECT CapturedReadRegion;
    PCHAR_INFO Ptr;

    if (Console == NULL || Buffer == NULL || CharInfo == NULL || ReadRegion == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Validity check */
    ASSERT(Console == Buffer->Header.Console);

    CapturedReadRegion = *ReadRegion;

    /* Make sure ReadRegion is inside the screen buffer */
    ConioInitRect(&ScreenBuffer, 0, 0,
                  Buffer->ScreenBufferSize.Y - 1,
                  Buffer->ScreenBufferSize.X - 1);
    if (!ConioGetIntersection(&CapturedReadRegion, &ScreenBuffer, &CapturedReadRegion))
    {
        /*
         * It is okay to have a ReadRegion completely outside
         * the screen buffer. No data is read then.
         */
        return STATUS_SUCCESS;
    }

    CurCharInfo = CharInfo;

    for (Y = CapturedReadRegion.Top; Y <= CapturedReadRegion.Bottom; ++Y)
    {
        Ptr = ConioCoordToPointer(Buffer, CapturedReadRegion.Left, Y);
        for (X = CapturedReadRegion.Left; X <= CapturedReadRegion.Right; ++X)
        {
            if (Unicode)
            {
                CurCharInfo->Char.UnicodeChar = Ptr->Char.UnicodeChar;
            }
            else
            {
                // ConsoleOutputUnicodeToAnsiChar(Console, &CurCharInfo->Char.AsciiChar, &Ptr->Char.UnicodeChar);
                WideCharToMultiByte(Console->OutputCodePage, 0, &Ptr->Char.UnicodeChar, 1,
                                    &CurCharInfo->Char.AsciiChar, 1, NULL, NULL);
            }
            CurCharInfo->Attributes = (Ptr->Attributes & ~COMMON_LEAD_TRAIL);
            ++Ptr;
            ++CurCharInfo;
        }
    }

    *ReadRegion = CapturedReadRegion;

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
ConDrvWriteConsoleOutput(IN PCONSOLE Console,
                         IN PTEXTMODE_SCREEN_BUFFER Buffer,
                         IN BOOLEAN Unicode,
                         IN PCHAR_INFO CharInfo/*Buffer*/,
                         IN OUT PSMALL_RECT WriteRegion)
{
    SHORT X, Y;
    SMALL_RECT ScreenBuffer;
    PCHAR_INFO CurCharInfo;
    SMALL_RECT CapturedWriteRegion;
    PCHAR_INFO Ptr;

    if (Console == NULL || Buffer == NULL || CharInfo == NULL || WriteRegion == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Validity check */
    ASSERT(Console == Buffer->Header.Console);

    CapturedWriteRegion = *WriteRegion;

    /* Make sure WriteRegion is inside the screen buffer */
    ConioInitRect(&ScreenBuffer, 0, 0,
                  Buffer->ScreenBufferSize.Y - 1,
                  Buffer->ScreenBufferSize.X - 1);
    if (!ConioGetIntersection(&CapturedWriteRegion, &ScreenBuffer, &CapturedWriteRegion))
    {
        /*
         * It is okay to have a WriteRegion completely outside
         * the screen buffer. No data is written then.
         */
        return STATUS_SUCCESS;
    }

    CurCharInfo = CharInfo;

    for (Y = CapturedWriteRegion.Top; Y <= CapturedWriteRegion.Bottom; ++Y)
    {
        Ptr = ConioCoordToPointer(Buffer, CapturedWriteRegion.Left, Y);
        for (X = CapturedWriteRegion.Left; X <= CapturedWriteRegion.Right; ++X)
        {
            if (Unicode)
            {
                Ptr->Char.UnicodeChar = CurCharInfo->Char.UnicodeChar;
            }
            else
            {
                ConsoleOutputAnsiToUnicodeChar(Console, &Ptr->Char.UnicodeChar, &CurCharInfo->Char.AsciiChar);
            }
            Ptr->Attributes = CurCharInfo->Attributes;
            ++Ptr;
            ++CurCharInfo;
        }
    }

    TermDrawRegion(Console, &CapturedWriteRegion);

    *WriteRegion = CapturedWriteRegion;

    return STATUS_SUCCESS;
}

/*
 * NOTE: This function is strongly inspired by ConDrvWriteConsoleOutput...
 * FIXME: This function MUST be moved into consrv/conoutput.c because only
 * consrv knows how to manipulate VDM screenbuffers.
 */
NTSTATUS NTAPI
ConDrvWriteConsoleOutputVDM(IN PCONSOLE Console,
                            IN PTEXTMODE_SCREEN_BUFFER Buffer,
                            IN PCHAR_CELL CharInfo/*Buffer*/,
                            IN COORD CharInfoSize,
                            IN PSMALL_RECT WriteRegion)
{
    SHORT X, Y;
    SMALL_RECT ScreenBuffer;
    PCHAR_CELL CurCharInfo;
    SMALL_RECT CapturedWriteRegion;
    PCHAR_INFO Ptr;

    if (Console == NULL || Buffer == NULL || CharInfo == NULL || WriteRegion == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Validity check */
    ASSERT(Console == Buffer->Header.Console);

    CapturedWriteRegion = *WriteRegion;

    /* Make sure WriteRegion is inside the screen buffer */
    ConioInitRect(&ScreenBuffer, 0, 0,
                  Buffer->ScreenBufferSize.Y - 1,
                  Buffer->ScreenBufferSize.X - 1);
    if (!ConioGetIntersection(&CapturedWriteRegion, &ScreenBuffer, &CapturedWriteRegion))
    {
        /*
         * It is okay to have a WriteRegion completely outside
         * the screen buffer. No data is written then.
         */
        return STATUS_SUCCESS;
    }

    // CurCharInfo = CharInfo;

    for (Y = CapturedWriteRegion.Top; Y <= CapturedWriteRegion.Bottom; ++Y)
    {
        /**/CurCharInfo = CharInfo + Y * CharInfoSize.X + CapturedWriteRegion.Left;/**/

        Ptr = ConioCoordToPointer(Buffer, CapturedWriteRegion.Left, Y);
        for (X = CapturedWriteRegion.Left; X <= CapturedWriteRegion.Right; ++X)
        {
            ConsoleOutputAnsiToUnicodeChar(Console, &Ptr->Char.UnicodeChar, &CurCharInfo->Char);
            Ptr->Attributes = CurCharInfo->Attributes;
            ++Ptr;
            ++CurCharInfo;
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
ConDrvWriteConsole(IN PCONSOLE Console,
                   IN PTEXTMODE_SCREEN_BUFFER ScreenBuffer,
                   IN BOOLEAN Unicode,
                   IN PVOID StringBuffer,
                   IN ULONG NumCharsToWrite,
                   OUT PULONG NumCharsWritten OPTIONAL)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PWCHAR Buffer = NULL;
    ULONG Written = 0;
    ULONG Length;

    if (Console == NULL || ScreenBuffer == NULL /* || StringBuffer == NULL */)
        return STATUS_INVALID_PARAMETER;

    /* Validity checks */
    ASSERT(Console == ScreenBuffer->Header.Console);
    ASSERT((StringBuffer != NULL) || (StringBuffer == NULL && NumCharsToWrite == 0));

    /* Stop here if the console is paused */
    if (Console->UnpauseEvent != NULL) return STATUS_PENDING;

    /* Convert the string to UNICODE */
    if (Unicode)
    {
        Buffer = StringBuffer;
    }
    else
    {
        Length = MultiByteToWideChar(Console->OutputCodePage, 0,
                                     (PCHAR)StringBuffer,
                                     NumCharsToWrite,
                                     NULL, 0);
        Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, Length * sizeof(WCHAR));
        if (Buffer)
        {
            MultiByteToWideChar(Console->OutputCodePage, 0,
                                (PCHAR)StringBuffer,
                                NumCharsToWrite,
                                (PWCHAR)Buffer, Length);
        }
        else
        {
            Status = STATUS_NO_MEMORY;
        }
    }

    /* Send it */
    if (Buffer)
    {
        if (NT_SUCCESS(Status))
        {
            Status = TermWriteStream(Console,
                                     ScreenBuffer,
                                     Buffer,
                                     NumCharsToWrite,
                                     TRUE);
            if (NT_SUCCESS(Status))
            {
                Written = NumCharsToWrite;
            }
        }

        if (!Unicode) RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
    }

    if (NumCharsWritten) *NumCharsWritten = Written;

    return Status;
}

NTSTATUS FASTCALL
IntReadConsoleOutputStringAscii(IN PCONSOLE Console,
                                IN PTEXTMODE_SCREEN_BUFFER Buffer,
                                OUT PVOID StringBuffer,
                                IN ULONG NumCodesToRead,
                                IN PCOORD ReadCoord,
                                OUT PULONG NumCodesRead OPTIONAL)
{
    ULONG CodeSize = RTL_FIELD_SIZE(CODE_ELEMENT, AsciiChar);
    LPBYTE ReadBuffer = StringBuffer;
    SHORT Xpos = ReadCoord->X;
    SHORT Ypos = (ReadCoord->Y + Buffer->VirtualY) % Buffer->ScreenBufferSize.Y;
    ULONG i;
    PCHAR_INFO Ptr;
    BOOLEAN bCJK = Console->IsCJK;

    for (i = 0; i < NumCodesToRead; ++i)
    {
        Ptr = ConioCoordToPointer(Buffer, Xpos, Ypos);

        ConsoleOutputUnicodeToAnsiChar(Console, (PCHAR)ReadBuffer, &Ptr->Char.UnicodeChar);
        ReadBuffer += CodeSize;

        Xpos++;
        if (Xpos == Buffer->ScreenBufferSize.X)
        {
            Xpos = 0;
            Ypos++;
            if (Ypos == Buffer->ScreenBufferSize.Y)
            {
                Ypos = 0;
            }
        }

        /* For Chinese, Japanese and Korean */
        if (bCJK && (Ptr->Attributes & COMMON_LVB_LEADING_BYTE))
        {
            Xpos++;
            if (Xpos == Buffer->ScreenBufferSize.X)
            {
                Xpos = 0;
                Ypos++;
                if (Ypos == Buffer->ScreenBufferSize.Y)
                {
                    Ypos = 0;
                }
            }
            ++i;
        }
    }

    if (NumCodesRead)
        *NumCodesRead = i;

    return STATUS_SUCCESS;
}

NTSTATUS FASTCALL
IntReadConsoleOutputStringUnicode(IN PCONSOLE Console,
                                  IN PTEXTMODE_SCREEN_BUFFER Buffer,
                                  OUT PVOID StringBuffer,
                                  IN ULONG NumCodesToRead,
                                  IN PCOORD ReadCoord,
                                  OUT PULONG NumCodesRead OPTIONAL)
{
    ULONG CodeSize = RTL_FIELD_SIZE(CODE_ELEMENT, UnicodeChar);
    LPBYTE ReadBuffer = StringBuffer;
    SHORT Xpos = ReadCoord->X;
    SHORT Ypos = (ReadCoord->Y + Buffer->VirtualY) % Buffer->ScreenBufferSize.Y;
    ULONG i, nNumChars = 0;
    PCHAR_INFO Ptr;
    BOOLEAN bCJK = Console->IsCJK;

    for (i = 0; i < NumCodesToRead; ++i, ++nNumChars)
    {
        Ptr = ConioCoordToPointer(Buffer, Xpos, Ypos);

        *(PWCHAR)ReadBuffer = Ptr->Char.UnicodeChar;
        ReadBuffer += CodeSize;

        Xpos++;
        if (Xpos == Buffer->ScreenBufferSize.X)
        {
            Xpos = 0;
            Ypos++;
            if (Ypos == Buffer->ScreenBufferSize.Y)
            {
                Ypos = 0;
            }
        }

        /* For Chinese, Japanese and Korean */
        if (bCJK && (Ptr->Attributes & COMMON_LVB_LEADING_BYTE))
        {
            Xpos++;
            if (Xpos == Buffer->ScreenBufferSize.X)
            {
                Xpos = 0;
                Ypos++;
                if (Ypos == Buffer->ScreenBufferSize.Y)
                {
                    Ypos = 0;
                }
            }
            ++i;
        }
    }

    if (NumCodesRead)
        *NumCodesRead = nNumChars;

    return STATUS_SUCCESS;
}

NTSTATUS FASTCALL
IntReadConsoleOutputStringAttributes(IN PCONSOLE Console,
                                     IN PTEXTMODE_SCREEN_BUFFER Buffer,
                                     OUT PVOID StringBuffer,
                                     IN ULONG NumCodesToRead,
                                     IN PCOORD ReadCoord,
                                     OUT PULONG NumCodesRead OPTIONAL)
{
    ULONG CodeSize = RTL_FIELD_SIZE(CODE_ELEMENT, Attribute);
    LPBYTE ReadBuffer = StringBuffer;
    SHORT Xpos = ReadCoord->X;
    SHORT Ypos = (ReadCoord->Y + Buffer->VirtualY) % Buffer->ScreenBufferSize.Y;
    ULONG i;
    PCHAR_INFO Ptr;

    for (i = 0; i < NumCodesToRead; ++i)
    {
        Ptr = ConioCoordToPointer(Buffer, Xpos, Ypos);

        *(PWORD)ReadBuffer = Ptr->Attributes;
        ReadBuffer += CodeSize;

        Xpos++;
        if (Xpos == Buffer->ScreenBufferSize.X)
        {
            Xpos = 0;
            Ypos++;
            if (Ypos == Buffer->ScreenBufferSize.Y)
            {
                Ypos = 0;
            }
        }
    }

    if (Xpos > 0 && Console->IsCJK)
    {
        ReadBuffer -= CodeSize;
        *(PWORD)ReadBuffer &= ~COMMON_LVB_LEADING_BYTE;
    }

    if (NumCodesRead)
        *NumCodesRead = NumCodesToRead;

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
ConDrvReadConsoleOutputString(IN PCONSOLE Console,
                              IN PTEXTMODE_SCREEN_BUFFER Buffer,
                              IN CODE_TYPE CodeType,
                              OUT PVOID StringBuffer,
                              IN ULONG NumCodesToRead,
                              IN PCOORD ReadCoord,
                              OUT PULONG NumCodesRead OPTIONAL)
{
    if (Console == NULL || Buffer == NULL || ReadCoord == NULL /* || EndCoord == NULL */)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Validity checks */
    ASSERT(Console == Buffer->Header.Console);
    ASSERT((StringBuffer != NULL) || (StringBuffer == NULL && NumCodesToRead == 0));

    if (NumCodesRead)
        *NumCodesRead = 0;

    switch (CodeType)
    {
        case CODE_ASCII:
            return IntReadConsoleOutputStringAscii(Console,
                                                   Buffer,
                                                   StringBuffer,
                                                   NumCodesToRead,
                                                   ReadCoord,
                                                   NumCodesRead);

        case CODE_UNICODE:
            return IntReadConsoleOutputStringUnicode(Console,
                                                     Buffer,
                                                     StringBuffer,
                                                     NumCodesToRead,
                                                     ReadCoord,
                                                     NumCodesRead);

        case CODE_ATTRIBUTE:
            return IntReadConsoleOutputStringAttributes(Console,
                                                        Buffer,
                                                        StringBuffer,
                                                        NumCodesToRead,
                                                        ReadCoord,
                                                        NumCodesRead);

        default:
            return STATUS_INVALID_PARAMETER;
    }
}

static NTSTATUS
IntWriteConsoleOutputStringUnicode(
    IN PCONSOLE Console,
    IN PTEXTMODE_SCREEN_BUFFER Buffer,
    IN PVOID StringBuffer,
    IN ULONG NumCodesToWrite,
    IN PCOORD WriteCoord,
    OUT PULONG NumCodesWritten OPTIONAL)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PWCHAR WriteBuffer = StringBuffer;
    ULONG i, X, Y, Length;
    PCHAR_INFO Ptr;
    BOOLEAN bCJK = Console->IsCJK;

    if (!StringBuffer)
        goto Cleanup;

    X = WriteCoord->X;
    Y = (WriteCoord->Y + Buffer->VirtualY) % Buffer->ScreenBufferSize.Y;
    Length = NumCodesToWrite;

    for (i = 0; i < Length; ++i)
    {
        Ptr = ConioCoordToPointer(Buffer, X, Y);

        Ptr->Char.UnicodeChar = *WriteBuffer;
        ++WriteBuffer;

        ++X;
        if (X == Buffer->ScreenBufferSize.X)
        {
            X = 0;
            ++Y;
            if (Y == Buffer->ScreenBufferSize.Y)
            {
                Y = 0;
            }
        }

        /* For Chinese, Japanese and Korean */
        if (bCJK && Ptr->Char.UnicodeChar >= 0x80 &&
            mk_wcwidth_cjk(Ptr->Char.UnicodeChar) == 2)
        {
            /* A full-width character cannot cross a line boundary */
            if (X == Buffer->ScreenBufferSize.X - 1)
            {
                /* go to next line */
                X = 0;
                ++Y;
                if (Y == Buffer->ScreenBufferSize.Y)
                {
                    Y = 0;
                }
                Ptr = ConioCoordToPointer(Buffer, X, Y);
            }

            /* the leading byte */
            Ptr->Attributes = Buffer->ScreenDefaultAttrib;
            Ptr->Attributes |= COMMON_LVB_LEADING_BYTE;
            ++i;

            /* the trailing byte */
            Ptr = ConioCoordToPointer(Buffer, X, Y);
            Ptr->Attributes = Buffer->ScreenDefaultAttrib;
            Ptr->Attributes |= COMMON_LVB_TRAILING_BYTE;

            ++X;
            if (X == Buffer->ScreenBufferSize.X)
            {
                X = 0;
                ++Y;
                if (Y == Buffer->ScreenBufferSize.Y)
                {
                    Y = 0;
                }
            }
        }
    }

Cleanup:
    if (NumCodesWritten)
        *NumCodesWritten = NumCodesToWrite;
    return Status;
}

static NTSTATUS
IntWriteConsoleOutputStringAscii(
    IN PCONSOLE Console,
    IN PTEXTMODE_SCREEN_BUFFER Buffer,
    IN PVOID StringBuffer,
    IN ULONG NumCodesToWrite,
    IN PCOORD WriteCoord,
    OUT PULONG NumCodesWritten OPTIONAL)
{
    NTSTATUS Status;
    PWCHAR tmpString;
    ULONG Length;

    if (!StringBuffer)
    {
        if (NumCodesWritten)
            *NumCodesWritten = NumCodesToWrite;

        return STATUS_SUCCESS;
    }

    /* Convert the ASCII string into Unicode before writing it to the console */
    Length = MultiByteToWideChar(Console->OutputCodePage, 0,
                                 StringBuffer,
                                 NumCodesToWrite,
                                 NULL, 0);
    tmpString = ConsoleAllocHeap(0, Length * sizeof(WCHAR));
    if (!tmpString)
        return STATUS_NO_MEMORY;

    MultiByteToWideChar(Console->OutputCodePage, 0,
                        StringBuffer,
                        NumCodesToWrite,
                        tmpString, Length);

    Status = IntWriteConsoleOutputStringUnicode(Console,
                                                Buffer,
                                                tmpString,
                                                Length,
                                                WriteCoord,
                                                NumCodesWritten);
    ConsoleFreeHeap(tmpString);
    return Status;
}

static NTSTATUS
IntWriteConsoleOutputStringAttribute(
    IN PCONSOLE Console,
    IN PTEXTMODE_SCREEN_BUFFER Buffer,
    IN PVOID StringBuffer,
    IN ULONG NumCodesToWrite,
    IN PCOORD WriteCoord,
    OUT PULONG NumCodesWritten OPTIONAL)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PWORD WriteBuffer = StringBuffer;
    ULONG i, X, Y, Length;
    PCHAR_INFO Ptr;

    if (!StringBuffer)
        goto Cleanup;

    X = WriteCoord->X;
    Y = (WriteCoord->Y + Buffer->VirtualY) % Buffer->ScreenBufferSize.Y;
    Length = NumCodesToWrite;

    for (i = 0; i < Length; ++i)
    {
        Ptr = ConioCoordToPointer(Buffer, X, Y);

        Ptr->Attributes = (*WriteBuffer & ~COMMON_LEAD_TRAIL);
        ++WriteBuffer;

        ++X;
        if (X == Buffer->ScreenBufferSize.X)
        {
            X = 0;
            ++Y;
            if (Y == Buffer->ScreenBufferSize.Y)
            {
                Y = 0;
            }
        }
    }

Cleanup:
    if (NumCodesWritten)
        *NumCodesWritten = NumCodesToWrite;
    return Status;
}

NTSTATUS NTAPI
ConDrvWriteConsoleOutputString(
    IN PCONSOLE Console,
    IN PTEXTMODE_SCREEN_BUFFER Buffer,
    IN CODE_TYPE CodeType,
    IN PVOID StringBuffer,
    IN ULONG NumCodesToWrite,
    IN PCOORD WriteCoord,
    OUT PULONG NumCodesWritten OPTIONAL)
{
    NTSTATUS Status;
    SMALL_RECT UpdateRect;

    if (Console == NULL || Buffer == NULL || WriteCoord == NULL /* || EndCoord == NULL */)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Validity checks */
    ASSERT(Console == Buffer->Header.Console);
    ASSERT((StringBuffer != NULL) || (StringBuffer == NULL && NumCodesToWrite == 0));

    if (NumCodesWritten)
        *NumCodesWritten = 0;

    switch (CodeType)
    {
        case CODE_ASCII:
            Status = IntWriteConsoleOutputStringAscii(
                Console, Buffer, StringBuffer, NumCodesToWrite, WriteCoord, NumCodesWritten);
            break;

        case CODE_UNICODE:
            Status = IntWriteConsoleOutputStringUnicode(
                Console, Buffer, StringBuffer, NumCodesToWrite, WriteCoord, NumCodesWritten);
            break;

        case CODE_ATTRIBUTE:
            Status = IntWriteConsoleOutputStringAttribute(
                Console, Buffer, StringBuffer, NumCodesToWrite, WriteCoord, NumCodesWritten);
            break;

        default:
            Status = STATUS_INVALID_PARAMETER;
            break;
    }

    if ((PCONSOLE_SCREEN_BUFFER)Buffer == Console->ActiveBuffer)
    {
        ConioComputeUpdateRect(Buffer, &UpdateRect, WriteCoord, NumCodesToWrite);
        TermDrawRegion(Console, &UpdateRect);
    }

    return Status;
}

NTSTATUS NTAPI
ConDrvFillConsoleOutput(IN PCONSOLE Console,
                        IN PTEXTMODE_SCREEN_BUFFER Buffer,
                        IN CODE_TYPE CodeType,
                        IN CODE_ELEMENT Code,
                        IN ULONG NumCodesToWrite,
                        IN PCOORD WriteCoord,
                        OUT PULONG NumCodesWritten OPTIONAL)
{
    ULONG X, Y, i;
    PCHAR_INFO Ptr;
    BOOLEAN bLead, bFullwidth;

    if (Console == NULL || Buffer == NULL || WriteCoord == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Validity check */
    ASSERT(Console == Buffer->Header.Console);

    //
    // FIXME: Make overflow checks on WriteCoord !!!!!!
    //

    if (NumCodesWritten) *NumCodesWritten = 0;

    if (CodeType == CODE_ASCII)
    {
        /* Conversion from the ASCII char to the UNICODE char */
        CODE_ELEMENT tmp;
        ConsoleOutputAnsiToUnicodeChar(Console, &tmp.UnicodeChar, &Code.AsciiChar);
        Code = tmp;
    }

    X = WriteCoord->X;
    Y = (WriteCoord->Y + Buffer->VirtualY) % Buffer->ScreenBufferSize.Y;
    // Ptr = ConioCoordToPointer(Buffer, X, Y); // Doesn't work
    // Ptr = &Buffer->Buffer[X + Y * Buffer->ScreenBufferSize.X]; // May work

    /* For Chinese, Japanese and Korean */
    bLead = TRUE;
    bFullwidth = FALSE;
    if (Console->IsCJK)
    {
        bFullwidth = (mk_wcwidth_cjk(Code.UnicodeChar) == 2);
        if (X > 0)
        {
            Ptr = ConioCoordToPointer(Buffer, X - 1, Y);
            if (Ptr->Attributes & COMMON_LVB_LEADING_BYTE)
            {
                Ptr->Char.UnicodeChar = L' ';
                Ptr->Attributes &= ~COMMON_LVB_LEADING_BYTE;
            }
        }
    }

    for (i = 0; i < NumCodesToWrite; ++i)
    {
        Ptr = ConioCoordToPointer(Buffer, X, Y);

        switch (CodeType)
        {
            case CODE_ASCII:
            case CODE_UNICODE:
                Ptr->Char.UnicodeChar = Code.UnicodeChar;
                Ptr->Attributes &= ~COMMON_LEAD_TRAIL;
                if (bFullwidth)
                {
                    if (bLead)
                        Ptr->Attributes |= COMMON_LVB_LEADING_BYTE;
                    else
                        Ptr->Attributes |= COMMON_LVB_TRAILING_BYTE;
                }
                break;

            case CODE_ATTRIBUTE:
                Ptr->Attributes &= ~COMMON_LEAD_TRAIL;
                Ptr->Attributes |= (Code.Attribute & ~COMMON_LEAD_TRAIL);
                break;
        }

        ++X;
        if (X == Buffer->ScreenBufferSize.X)
        {
            X = 0;
            ++Y;
            if (Y == Buffer->ScreenBufferSize.Y)
            {
                Y = 0;
            }
        }

        bLead = !bLead;
    }

    if ((NumCodesToWrite & 1) & bFullwidth)
    {
        if (X + Y * Buffer->ScreenBufferSize.X > 0)
        {
            Ptr = ConioCoordToPointer(Buffer, X - 1, Y);
            Ptr->Char.UnicodeChar = L' ';
            Ptr->Attributes &= ~COMMON_LEAD_TRAIL;
        }
    }

    if ((PCONSOLE_SCREEN_BUFFER)Buffer == Console->ActiveBuffer)
    {
        SMALL_RECT UpdateRect;
        ConioComputeUpdateRect(Buffer, &UpdateRect, WriteCoord, NumCodesToWrite);
        TermDrawRegion(Console, &UpdateRect);
    }

    if (NumCodesWritten) *NumCodesWritten = NumCodesToWrite; // Written;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
ConDrvGetConsoleScreenBufferInfo(IN  PCONSOLE Console,
                                 IN  PTEXTMODE_SCREEN_BUFFER Buffer,
                                 OUT PCOORD ScreenBufferSize,
                                 OUT PCOORD CursorPosition,
                                 OUT PCOORD ViewOrigin,
                                 OUT PCOORD ViewSize,
                                 OUT PCOORD MaximumViewSize,
                                 OUT PWORD  Attributes)
{
    COORD LargestWindowSize;

    if (Console == NULL || Buffer == NULL || ScreenBufferSize == NULL ||
        CursorPosition  == NULL || ViewOrigin == NULL || ViewSize == NULL ||
        MaximumViewSize == NULL || Attributes == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Validity check */
    ASSERT(Console == Buffer->Header.Console);

    *ScreenBufferSize = Buffer->ScreenBufferSize;
    *CursorPosition   = Buffer->CursorPosition;
    *ViewOrigin       = Buffer->ViewOrigin;
    *ViewSize         = Buffer->ViewSize;
    *Attributes       = Buffer->ScreenDefaultAttrib;

    /*
     * Retrieve the largest possible console window size, taking
     * into account the size of the console screen buffer.
     */
    TermGetLargestConsoleWindowSize(Console, &LargestWindowSize);
    LargestWindowSize.X = min(LargestWindowSize.X, Buffer->ScreenBufferSize.X);
    LargestWindowSize.Y = min(LargestWindowSize.Y, Buffer->ScreenBufferSize.Y);
    *MaximumViewSize = LargestWindowSize;

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
ConDrvSetConsoleTextAttribute(IN PCONSOLE Console,
                              IN PTEXTMODE_SCREEN_BUFFER Buffer,
                              IN WORD Attributes)
{
    if (Console == NULL || Buffer == NULL)
        return STATUS_INVALID_PARAMETER;

    /* Validity check */
    ASSERT(Console == Buffer->Header.Console);

    Buffer->ScreenDefaultAttrib = Attributes;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
ConDrvSetConsoleScreenBufferSize(IN PCONSOLE Console,
                                 IN PTEXTMODE_SCREEN_BUFFER Buffer,
                                 IN PCOORD Size)
{
    NTSTATUS Status;

    if (Console == NULL || Buffer == NULL || Size == NULL)
        return STATUS_INVALID_PARAMETER;

    /* Validity check */
    ASSERT(Console == Buffer->Header.Console);

    Status = ConioResizeBuffer(Console, Buffer, *Size);
    if (NT_SUCCESS(Status)) TermResizeTerminal(Console);

    return Status;
}

NTSTATUS NTAPI
ConDrvScrollConsoleScreenBuffer(IN PCONSOLE Console,
                                IN PTEXTMODE_SCREEN_BUFFER Buffer,
                                IN BOOLEAN Unicode,
                                IN PSMALL_RECT ScrollRectangle,
                                IN BOOLEAN UseClipRectangle,
                                IN PSMALL_RECT ClipRectangle OPTIONAL,
                                IN PCOORD DestinationOrigin,
                                IN CHAR_INFO FillChar)
{
    COORD CapturedDestinationOrigin;
    SMALL_RECT ScreenBuffer;
    SMALL_RECT SrcRegion;
    SMALL_RECT DstRegion;
    SMALL_RECT UpdateRegion;
    SMALL_RECT CapturedClipRectangle;

    if (Console == NULL || Buffer == NULL || ScrollRectangle == NULL ||
        (UseClipRectangle ? ClipRectangle == NULL : FALSE) || DestinationOrigin == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Validity check */
    ASSERT(Console == Buffer->Header.Console);

    CapturedDestinationOrigin = *DestinationOrigin;

    /* Make sure the source rectangle is inside the screen buffer */
    ConioInitRect(&ScreenBuffer, 0, 0,
                  Buffer->ScreenBufferSize.Y - 1,
                  Buffer->ScreenBufferSize.X - 1);
    if (!ConioGetIntersection(&SrcRegion, &ScreenBuffer, ScrollRectangle))
    {
        return STATUS_SUCCESS;
    }

    /* If the source was clipped on the left or top, adjust the destination accordingly */
    if (ScrollRectangle->Left < 0)
    {
        CapturedDestinationOrigin.X -= ScrollRectangle->Left;
    }
    if (ScrollRectangle->Top < 0)
    {
        CapturedDestinationOrigin.Y -= ScrollRectangle->Top;
    }

    if (UseClipRectangle)
    {
        CapturedClipRectangle = *ClipRectangle;
        if (!ConioGetIntersection(&CapturedClipRectangle, &CapturedClipRectangle, &ScreenBuffer))
        {
            return STATUS_SUCCESS;
        }
    }
    else
    {
        CapturedClipRectangle = ScreenBuffer;
    }

    ConioInitRect(&DstRegion,
                  CapturedDestinationOrigin.Y,
                  CapturedDestinationOrigin.X,
                  CapturedDestinationOrigin.Y + ConioRectHeight(&SrcRegion) - 1,
                  CapturedDestinationOrigin.X + ConioRectWidth(&SrcRegion ) - 1);

    if (!Unicode)
    {
        WCHAR tmp;
        ConsoleOutputAnsiToUnicodeChar(Console, &tmp, &FillChar.Char.AsciiChar);
        FillChar.Char.UnicodeChar = tmp;
    }

    ConioMoveRegion(Buffer, &SrcRegion, &DstRegion, &CapturedClipRectangle, FillChar);

    if ((PCONSOLE_SCREEN_BUFFER)Buffer == Console->ActiveBuffer)
    {
        ConioGetUnion(&UpdateRegion, &SrcRegion, &DstRegion);
        if (ConioGetIntersection(&UpdateRegion, &UpdateRegion, &CapturedClipRectangle))
        {
            /* Draw update region */
            TermDrawRegion(Console, &UpdateRegion);
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
ConDrvSetConsoleWindowInfo(IN PCONSOLE Console,
                           IN PTEXTMODE_SCREEN_BUFFER Buffer,
                           IN BOOLEAN Absolute,
                           IN PSMALL_RECT WindowRect)
{
    SMALL_RECT CapturedWindowRect;
    COORD LargestWindowSize;

    if (Console == NULL || Buffer == NULL || WindowRect == NULL)
        return STATUS_INVALID_PARAMETER;

    /* Validity check */
    ASSERT(Console == Buffer->Header.Console);

    CapturedWindowRect = *WindowRect;

    if (!Absolute)
    {
        /* Relative positions are given, transform them to absolute ones */
        CapturedWindowRect.Left   += Buffer->ViewOrigin.X;
        CapturedWindowRect.Top    += Buffer->ViewOrigin.Y;
        CapturedWindowRect.Right  += Buffer->ViewOrigin.X + Buffer->ViewSize.X - 1;
        CapturedWindowRect.Bottom += Buffer->ViewOrigin.Y + Buffer->ViewSize.Y - 1;
    }

    /*
     * The MSDN documentation on SetConsoleWindowInfo is partially wrong about
     * the performed checks this API performs. While it is correct that the
     * 'Right'/'Bottom' members cannot be strictly smaller than the 'Left'/'Top'
     * members, they can be equal.
     * Also, if the 'Left' or 'Top' members are negative, this is automatically
     * corrected for, and the window rectangle coordinates are shifted accordingly.
     */
    if ((CapturedWindowRect.Right  < CapturedWindowRect.Left) ||
        (CapturedWindowRect.Bottom < CapturedWindowRect.Top))
    {
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * Forbid window sizes larger than the largest allowed console window size,
     * taking into account the size of the console screen buffer.
     */
    TermGetLargestConsoleWindowSize(Console, &LargestWindowSize);
    LargestWindowSize.X = min(LargestWindowSize.X, Buffer->ScreenBufferSize.X);
    LargestWindowSize.Y = min(LargestWindowSize.Y, Buffer->ScreenBufferSize.Y);
    if ((CapturedWindowRect.Right - CapturedWindowRect.Left + 1 > LargestWindowSize.X) ||
        (CapturedWindowRect.Bottom - CapturedWindowRect.Top + 1 > LargestWindowSize.Y))
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Shift the window rectangle coordinates if 'Left' or 'Top' are negative */
    if (CapturedWindowRect.Left < 0)
    {
        CapturedWindowRect.Right -= CapturedWindowRect.Left;
        CapturedWindowRect.Left = 0;
    }
    if (CapturedWindowRect.Top < 0)
    {
        CapturedWindowRect.Bottom -= CapturedWindowRect.Top;
        CapturedWindowRect.Top = 0;
    }

    /* Clip the window rectangle to the screen buffer */
    CapturedWindowRect.Right  = min(CapturedWindowRect.Right , Buffer->ScreenBufferSize.X);
    CapturedWindowRect.Bottom = min(CapturedWindowRect.Bottom, Buffer->ScreenBufferSize.Y);

    Buffer->ViewOrigin.X = CapturedWindowRect.Left;
    Buffer->ViewOrigin.Y = CapturedWindowRect.Top;

    Buffer->ViewSize.X = CapturedWindowRect.Right - CapturedWindowRect.Left + 1;
    Buffer->ViewSize.Y = CapturedWindowRect.Bottom - CapturedWindowRect.Top + 1;

    TermResizeTerminal(Console);

    return STATUS_SUCCESS;
}

/* EOF */
