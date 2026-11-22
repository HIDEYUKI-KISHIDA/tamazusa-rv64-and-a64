10 ' ARM.BAS  3 関節のロボットアーム（仮想のサーボ 0〜2）
20 ' つかむ → 持ち上げる → 回す → 置く、をくり返す。どれかのキーで終わる
30 DIM P(3,2): ESTOP 0
40 FOR I=0 TO 3: FOR J=0 TO 2: READ P(I,J): NEXT J,I
50 DATA 0,-30,0,  0,30,60,  90,30,60,  90,-30,0
60 DO
70   FOR I=0 TO 3
80     CALL GOTO3(P(I,0), P(I,1), P(I,2), 90)
90     IF INKEY$<>"" THEN EXIT DO
100  NEXT I
110 LOOP
120 PRINT "おわり": END
200 SUB GOTO3(A, B, C, V)
210   LOCAL K
220   SERVO 0,A,V: SERVO 1,B: SERVO 2,C
230   DO
240     SLEEP 100
250     PRINT USING "肩 ####.#  ひじ ####.#  手首 ####.#"; SERVO(0); SERVO(1); SERVO(2)
260     K=ABS(SERVO(0)-A)+ABS(SERVO(1)-B)+ABS(SERVO(2)-C)
270   LOOP WHILE K > .5
280 END SUB
