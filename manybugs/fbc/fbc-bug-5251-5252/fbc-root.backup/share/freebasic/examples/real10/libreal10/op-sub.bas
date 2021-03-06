#include once "real10.bi"
#include once "intern.bi"

Operator - ( Byref lhs As ext, Byref rhs As ext ) As ext
    Dim As ext retval
    x_Sub ( retval, lhs, rhs )
    Return retval
End Operator

Operator - ( Byref lhs As ext, Byval rhs As Integer ) As ext
    Dim As ext retval
    ireal10 ( retval, rhs )
    x_Sub ( retval, lhs, retval )
    Return retval
End Operator

Operator - ( Byval lhs As Integer, Byref rhs As ext ) As ext
    Dim As ext retval
    iReal10 ( retval, lhs )
    x_Sub ( retval, retval, rhs )
    Return retval
End Operator

Operator - ( Byref lhs As ext, Byval rhs As Single ) As ext
    Dim As ext retval
    sreal10 ( retval, rhs )
    x_Sub ( retval, lhs, retval )
    Return retval
End Operator

Operator - ( Byval lhs As Single, Byref rhs As ext ) As ext
    Dim As ext retval
    sReal10 ( retval, lhs )
    x_Sub ( retval, retval, rhs )
    Return retval
End Operator

Operator - ( Byref lhs As ext, Byval rhs As Double ) As ext
    Dim As ext retval
    dreal10 ( retval, rhs )
    x_Sub ( retval, lhs, retval )
    Return retval
End Operator

Operator - ( Byval lhs As Double, Byref rhs As ext ) As ext
    Dim As ext retval
    dReal10 ( retval, lhs )
    x_Sub ( retval, retval, rhs )
    Return retval
End Operator

Operator - ( Byref lhs As ext ) As ext
    Dim As ext retval
    x_Neg ( retval, lhs )
    Return retval
End Operator
