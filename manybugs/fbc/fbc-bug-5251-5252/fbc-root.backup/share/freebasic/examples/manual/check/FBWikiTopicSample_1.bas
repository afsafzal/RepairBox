'' examples/manual/check/FBWikiTopicSample_1.bas
''
'' NOTICE: This file is part of the FreeBASIC Compiler package and can't
''         be included in other distributions without authorization.
''
'' See Also: http://www.freebasic.net/wiki/wikka.php?wakka=FBWikiTopicSample
'' --------

Const param1 As String = "a string"
Const param2 As Integer = &Hdeadbeef

Function func( a As String, b As Integer = 0) As Integer
	Return Val(a)+b
End Function

	'' acquire result
	Dim As Integer result
	result = func( param1 )
	
	'' acquire new result with param2
	result = func( param1, param2 )
