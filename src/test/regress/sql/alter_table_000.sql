--
--- ALTER TABLE related tickets or bugs fixed
--
-- ATLER TABLE SYNTAX
\h alter table
--custom script
--create table
CREATE TABLE TBL_DOMAIN
(
  IDOMAINID   NUMBER(10) NOT NULL,
  SDOMAINNAME VARCHAR2(30) NOT NULL
);
--create/recreate primary, unique and foreign key constraints 
ALTER TABLE TBL_DOMAIN
  ADD CONSTRAINT PK_TBL_DOMAIN PRIMARY KEY (IDOMAINID)
  USING INDEX ;
  
ALTER TABLE TBL_DOMAIN
  ADD CONSTRAINT IX_TBL_DOMAIN UNIQUE (SDOMAINNAME)
  USING INDEX ;
\d+ TBL_DOMAIN
DROP TABLE TBL_DOMAIN;

--create table
CREATE TABLE TBL_CM_MAXTSENDTOHOST
(
  I_MODULETYPE  NUMBER(38) NOT NULL,
  I_MODULENO    NUMBER(38) NOT NULL,
  I_PLAMODULENO NUMBER(38) NOT NULL,
  I_TABLEID     NUMBER(38) NOT NULL,
  I_OLDMAXTUPLE NUMBER(38) NOT NULL,
  I_NEWMAXTUPLE NUMBER(38) NOT NULL,
  I_RESERVED1   NUMBER(38) DEFAULT 0,
  I_RESERVED2   NUMBER(38) DEFAULT 0,
  I_RESERVED3   NUMBER(38) DEFAULT 0,
  I_RESERVED4   NUMBER(38) DEFAULT 0,
  I_RESERVED5   NUMBER(38) DEFAULT 0,
  I_RESERVED6   NUMBER(38) DEFAULT 0,
  I_RESERVED7   NUMBER(38) DEFAULT 0,
  SV_RESERVED8  VARCHAR2(32) DEFAULT '',
  SV_RESERVED9  VARCHAR2(32) DEFAULT '',
  SV_RESERVED10 VARCHAR2(32) DEFAULT ''
)
  PCTFREE 10
  INITRANS 1
  MAXTRANS 255
  STORAGE
  (
    INITIAL 64K
    MINEXTENTS 1
    MAXEXTENTS UNLIMITED
  );
--add primary key
ALTER TABLE TBL_CM_MAXTSENDTOHOST
  ADD PRIMARY KEY (I_PLAMODULENO, I_TABLEID)
  USING INDEX 
  PCTFREE 10
  INITRANS 2
  MAXTRANS 255
  STORAGE
  (
    INITIAL 64K
    MINEXTENTS 1
    MAXEXTENTS UNLIMITED
  );
 \d+ TBL_CM_MAXTSENDTOHOST
 DROP TABLE TBL_CM_MAXTSENDTOHOST;

--create table
CREATE TABLE TBL_LICCTRLDESC_DEFAULT
(
  I_INDEX        NUMBER(38) NOT NULL,
  SV_FEATURENAME VARCHAR2(64) NOT NULL,
  SV_ITEMNAME    VARCHAR2(64) NOT NULL,
  I_ITEMTYPE     NUMBER(38) NOT NULL,
  I_ITEMVALUEMIN NUMBER(38) NOT NULL,
  I_ITEMVALUEMAX NUMBER(38) NOT NULL,
  I_RESERVED1    NUMBER(38) DEFAULT 0,
  I_RESERVED2    NUMBER(38) DEFAULT 0,
  I_RESERVED3    NUMBER(38) DEFAULT 0,
  I_RESERVED4    NUMBER(38) DEFAULT 0,
  I_RESERVED5    NUMBER(38) DEFAULT 0,
  I_RESERVED6    NUMBER(38) DEFAULT 0,
  I_RESERVED7    NUMBER(38) DEFAULT 0,
  SV_RESERVED8   VARCHAR2(32) DEFAULT '',
  SV_RESERVED9   VARCHAR2(32) DEFAULT '',
  SV_RESERVED10  VARCHAR2(32) DEFAULT '',
  I_STATUS       NUMBER(38) NOT NULL
)
  PCTFREE 10
  INITRANS 1
  MAXTRANS 255
  STORAGE
  (
    INITIAL 64K
    MINEXTENTS 1
    MAXEXTENTS UNLIMITED
  );
--add primary key
ALTER TABLE TBL_LICCTRLDESC_DEFAULT
  ADD PRIMARY KEY (I_INDEX)
  USING INDEX 
  PCTFREE 10
  INITRANS 2
  MAXTRANS 255
  STORAGE
  (
    INITIAL 64K
    MINEXTENTS 1
    MAXEXTENTS UNLIMITED
  );
--add unique index
CREATE UNIQUE INDEX IDX_TBL_LICCTRL_DEF ON TBL_LICCTRLDESC_DEFAULT (I_INDEX DESC, I_STATUS)
  PCTFREE 10
  INITRANS 2
  MAXTRANS 255
  STORAGE
  (
    INITIAL 64K
    MINEXTENTS 1
    MAXEXTENTS UNLIMITED
  );
\d+ TBL_LICCTRLDESC_DEFAULT
 DROP TABLE TBL_LICCTRLDESC_DEFAULT;
--using index clause
CREATE TABLE STUDENTS
(
	ID INT,
	NAME VARCHAR2(20),
	AGE INT,
	ADDRESS VARCHAR(30)
);
 --alter table to add unique index or primary key 
ALTER TABLE STUDENTS ADD UNIQUE (ID)
USING INDEX
PCTFREE 10
INITRANS 2
MAXTRANS 255
STORAGE
(
  INITIAL 64K
  MINEXTENTS 1
  MAXEXTENTS UNLIMITED
);

ALTER TABLE STUDENTS ADD CONSTRAINT ZHANGYG UNIQUE (AGE, ADDRESS)
USING INDEX
PCTFREE 10
INITRANS 2
MAXTRANS 255
STORAGE
(
  INITIAL 64K
  MINEXTENTS 1
  MAXEXTENTS UNLIMITED
);

ALTER TABLE STUDENTS ADD PRIMARY KEY (AGE)
USING INDEX
PCTFREE 10
INITRANS 2
MAXTRANS 255
STORAGE
(
  INITIAL 64K
  MINEXTENTS 1
  MAXEXTENTS UNLIMITED
);
\d+ STUDENTS
DROP TABLE STUDENTS;
--simulate a's ALTER TABLE gram
CREATE TABLE MODIFY_TABLE_A(I INTEGER);
ALTER TABLE MODIFY_TABLE_A ADD (mychar CHAR); 
ALTER TABLE MODIFY_TABLE_A ADD (myint1 INT, mychar1 CHAR);
ALTER TABLE MODIFY_TABLE_A ADD (myint2 INT, mychar2 CHAR, mychar3 CHAR);
ALTER TABLE MODIFY_TABLE_A ADD a CHAR, ADD b CHAR;
\d MODIFY_TABLE_A
ALTER TABLE MODIFY_TABLE_A ADD mychar4 CHAR;
\d MODIFY_TABLE_A
ALTER TABLE MODIFY_TABLE_A MODIFY I VARCHAR2(64);
\d MODIFY_TABLE_A
ALTER TABLE MODIFY_TABLE_A MODIFY I CHAR, MODIFY myint1 CHAR;
\d MODIFY_TABLE_A
ALTER TABLE MODIFY_TABLE_A MODIFY (myint1 VARCHAR(12));
\d MODIFY_TABLE_A
ALTER TABLE MODIFY_TABLE_A MODIFY (myint1 VARCHAR(13), mychar1 INT);
\d MODIFY_TABLE_A
DROP TABLE MODIFY_TABLE_A;

create table test_alter_type(a int,b text);
alter table test_alter_type alter column a type regclass;
DROP TABLE test_alter_type;

create table test_mod(a int,b text);
alter table test_mod alter column a type regclass;
alter table test_mod alter column a set default "d";
alter table test_mod alter column a set default "d"::int;
alter table test_mod alter column a set default "d"::int + 1;
drop table test_mod;

--simulate a and postgresql, ALTER TABLE IF EXISTS table_name ADD( { element_list_clause } [, ...] )
--simulate a and postgresql, ALTER TABLE IF EXISTS table_name MODIFY( { element_list_clause } [, ...] )
create schema  columnar_storage;
create table columnar_storage.create_columnar_add_common_008 (c_tinyint  tinyint,c_smallint smallint,c_int integer,c_bigint   bigint,c_money    money,c_numeric   numeric,c_real      real,c_double    double precision,c_decimal   decimal,c_varchar   varchar,c_char   char(30),c_nvarchar2  nvarchar2,c_text text,c_timestamp   timestamp with time zone,c_timestamptz timestamp without time zone,c_date     date,c_time     time without time zone,c_timetz   time with time zone,c_interval  interval,c_tinterval   tinterval,c_smalldatetime   smalldatetime,c_bytea   bytea,c_boolean  boolean,c_inet inet,c_cidr cidr,c_bit bit(10),c_varbit varbit(10),c_oid oid) with (orientation=column);
alter table if exists columnar_storage.create_columnar_add_common_007 modify (c_int varchar(20));
alter table if exists columnar_storage.create_columnar_add_common_008 modify (c_int varchar(20), c_double  varchar(20));
select * from columnar_storage.create_columnar_add_common_008;
drop table columnar_storage.create_columnar_add_common_008;
create table columnar_storage.create_columnar_add_common_008 (c_tinyint  tinyint,c_smallint smallint,c_int integer,c_bigint   bigint,c_money    money,c_numeric   numeric,c_real      real,c_double    double precision,c_decimal   decimal,c_varchar   varchar,c_char   char(30),c_nvarchar2  nvarchar2,c_text text,c_timestamp   timestamp with time zone,c_timestamptz timestamp without time zone,c_date     date,c_time     time without time zone,c_timetz   time with time zone,c_interval  interval,c_tinterval   tinterval,c_smalldatetime   smalldatetime,c_bytea   bytea,c_boolean  boolean,c_inet inet,c_cidr cidr,c_bit bit(10),c_varbit varbit(10),c_oid oid) with (orientation=column);
alter table if exists columnar_storage.create_columnar_add_common_007 add (c_time_008 time without time zone,c_timetz_008  time with time zone);
alter table if exists columnar_storage.create_columnar_add_common_008 add (c_time_008 time without time zone,c_timetz_008  time with time zone);
select * from columnar_storage.create_columnar_add_common_008;
drop table columnar_storage.create_columnar_add_common_008;
drop schema columnar_storage;

create table test_drop_column_1 (a int, b int, c int);
create table test_drop_column_2 (a int, b int);
create table test_drop_column_3 (a int, b int);
alter table test_drop_column_1 drop column c;
explain (verbose true, costs false) insert into test_drop_column_1 select * from test_drop_column_2;
insert into test_drop_column_1 select * from test_drop_column_2;
explain (verbose true, costs false) insert into test_drop_column_1 select * from test_drop_column_2 order by 2;
insert into test_drop_column_1 select * from test_drop_column_2 order by 2;
explain (verbose true, costs false) insert into test_drop_column_1 select test_drop_column_2.a, test_drop_column_3.a from test_drop_column_2, test_drop_column_3 where test_drop_column_2.a = test_drop_column_3.a;
insert into test_drop_column_1 select test_drop_column_2.a, test_drop_column_3.a from test_drop_column_2, test_drop_column_3 where test_drop_column_2.a = test_drop_column_3.a;
explain (verbose true, costs false) insert into test_drop_column_1 select test_drop_column_2.a, test_drop_column_3.a from test_drop_column_2, test_drop_column_3 where test_drop_column_2.a = test_drop_column_3.b;
insert into test_drop_column_1 select test_drop_column_2.a, test_drop_column_3.a from test_drop_column_2, test_drop_column_3 where test_drop_column_2.a = test_drop_column_3.b;
explain (verbose true, costs false) insert into test_drop_column_1 select test_drop_column_2.a, test_drop_column_3.a from test_drop_column_2, test_drop_column_3 where test_drop_column_2.a = test_drop_column_3.b order by 1, 2;
insert into test_drop_column_1 select test_drop_column_2.a, test_drop_column_3.a from test_drop_column_2, test_drop_column_3 where test_drop_column_2.a = test_drop_column_3.b order by 1, 2;
explain (verbose true, costs false) update test_drop_column_1 set a=test_drop_column_2.a from test_drop_column_2;
update test_drop_column_1 set a=test_drop_column_2.a from test_drop_column_2;
explain (verbose true, costs false) delete from test_drop_column_1 where a in (select a from test_drop_column_2);
delete from test_drop_column_1 where a in (select a from test_drop_column_2);

create table test_drop_column_cstore_1 (a int, b int, c int) with (orientation = column);
create table test_drop_column_cstore_2 (a int, b int) with (orientation = column);
create table test_drop_column_cstore_3 (a int) with (orientation = column);
alter table test_drop_column_cstore_1 drop column c;
insert into test_drop_column_cstore_1 select * from test_drop_column_cstore_2;
insert into test_drop_column_cstore_1 select * from test_drop_column_cstore_2 order by 2;
insert into test_drop_column_cstore_1 select test_drop_column_cstore_2.a, test_drop_column_cstore_3.a from test_drop_column_cstore_2, test_drop_column_cstore_3 where test_drop_column_cstore_2.a = test_drop_column_cstore_3.a;

drop table test_drop_column_1;
drop table test_drop_column_2;
drop table test_drop_column_3;
drop table test_drop_column_cstore_1;
drop table test_drop_column_cstore_2;
drop table test_drop_column_cstore_3;

create table test_hash (a int, b int);
create sequence test_seq1;
alter table test_hash alter column a type serial; --fail 
alter table test_hash alter column a set default nextval('test_seq1'); 
insert into test_hash(b) values(generate_series(1,10));
alter table test_hash add column c serial; --not supported
alter table test_hash add column d int default nextval('test_seq1'); --not supported
alter table test_hash add column e int default nextval('test_seq1')*10; --not supported
drop table test_hash;
drop sequence test_seq1;

-- check column addition within a view (bug #14876)
create table at_base_table(id int, stuff text);
insert into at_base_table values (23, 'skidoo');
create view at_view_1 as select * from at_base_table bt;
create view at_view_2 as select *, v1 as j from at_view_1 v1;
\d+ at_view_1
\d+ at_view_2
explain (verbose, costs off) select * from at_view_2;
select * from at_view_2;

create or replace view at_view_1 as select *, 2+2 as more from at_base_table bt;
\d+ at_view_1
\d+ at_view_2
explain (verbose, costs off) select * from at_view_2;
select * from at_view_2;

drop view at_view_2;
drop view at_view_1;
drop table at_base_table;

create table tt_row_rep_1(a int);
alter table tt_row_rep_1 drop column a;

create table tt_row_rep_2(a int, b int);
alter table tt_row_rep_2 drop column b;
alter table tt_row_rep_2 drop column a;

create table tt_col_rep_1(a int) with(orientation=column);
alter table tt_col_rep_1 drop column a;

create table tt_col_rep_2(a int, b int) with(orientation=column);
alter table tt_col_rep_2 drop column b;
alter table tt_col_rep_2 drop column a;

drop table tt_row_rep_1;
drop table tt_row_rep_2;
drop table tt_col_rep_1;
drop table tt_col_rep_2;
