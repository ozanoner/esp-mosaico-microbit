// Second PoC payload: proves the program hash moves when the body changes while
// the DAL/template hash stays put.
const REVISION = "B2"

bluetooth.startUartService()
basic.showString(REVISION)

input.onButtonPressed(Button.A, function () {
    basic.showNumber(Math.randomRange(1, 6))
})
