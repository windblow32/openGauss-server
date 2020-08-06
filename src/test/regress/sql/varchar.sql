--
-- VARCHAR
--

CREATE TABLE VARCHAR_TBL(f1 varchar(1));

INSERT INTO VARCHAR_TBL (f1) VALUES ('a');

INSERT INTO VARCHAR_TBL (f1) VALUES ('A');

-- any of the following three input formats are acceptable
INSERT INTO VARCHAR_TBL (f1) VALUES ('1');

INSERT INTO VARCHAR_TBL (f1) VALUES (2);

INSERT INTO VARCHAR_TBL (f1) VALUES ('3');

-- zero-length char
INSERT INTO VARCHAR_TBL (f1) VALUES ('');

-- try varchar's of greater than 1 length
INSERT INTO VARCHAR_TBL (f1) VALUES ('cd');
INSERT INTO VARCHAR_TBL (f1) VALUES ('c     ');


SELECT '' AS seven, * FROM VARCHAR_TBL ORDER BY f1;

SELECT '' AS six, c.*
   FROM VARCHAR_TBL c
   WHERE c.f1 <> 'a' ORDER BY f1;

SELECT '' AS one, c.*
   FROM VARCHAR_TBL c
   WHERE c.f1 = 'a';

SELECT '' AS five, c.*
   FROM VARCHAR_TBL c
   WHERE c.f1 < 'a' ORDER BY f1;

SELECT '' AS six, c.*
   FROM VARCHAR_TBL c
   WHERE c.f1 <= 'a' ORDER BY f1;

SELECT '' AS one, c.*
   FROM VARCHAR_TBL c
   WHERE c.f1 > 'a' ORDER BY f1;

SELECT '' AS two, c.*
   FROM VARCHAR_TBL c
   WHERE c.f1 >= 'a' ORDER BY f1;

DROP TABLE VARCHAR_TBL;

--
-- Now test longer arrays of char
--

CREATE TABLE VARCHAR_TBL(f1 varchar(4));

INSERT INTO VARCHAR_TBL (f1) VALUES ('a');
INSERT INTO VARCHAR_TBL (f1) VALUES ('ab');
INSERT INTO VARCHAR_TBL (f1) VALUES ('abcd');
INSERT INTO VARCHAR_TBL (f1) VALUES ('abcde');
INSERT INTO VARCHAR_TBL (f1) VALUES ('abcd    ');

SELECT '' AS four, * FROM VARCHAR_TBL ORDER BY f1;

select char_length(to_char(lpad('abc', 1024 * 1024 *10, 'x')));

select char_length(to_char(lpad('abc', 1024 * 1024 *10 + 1, 'x')));
