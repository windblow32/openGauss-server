--
---- test global index
--

drop table if exists hw_global_index_rp;
create table hw_global_index_rp
(
	c1 int,
	c2 int
)
partition by range (c1)
(
	partition hw_global_index_rp_p0 values less than (50),
	partition hw_global_index_rp_p1 values less than (100),
	partition hw_global_index_rp_p2 values less than (150)
);
--succeed

insert into hw_global_index_rp values(1, 1);
insert into hw_global_index_rp values(49, 2);
insert into hw_global_index_rp values(50, 3);
insert into hw_global_index_rp values(100, 4);
insert into hw_global_index_rp values(149, 5);

create index rp_index_global1 on hw_global_index_rp (c1) global;
--succeed

create index on hw_global_index_rp (c1) global;
--fail

create unique index rp_index_global2 on hw_global_index_rp (c1) global;
--succeed

create unique index on hw_global_index_rp (c1) global;
--fail

--expression
create index rp_index_global3 on hw_global_index_rp ((c1+c2)) global;
--fail

create index rp_index_global4 on hw_global_index_rp ((c1-c2)) global;
--succeed

create index rp_index_global15 on hw_global_index_rp ((c1-c2),c1,c2) global;
--succeed

create index rp_index_global16 on hw_global_index_rp using hash (c1) global;
--fail ERROR: access method "HASH" does not support global indexes

create index rp_index_global17 on hw_global_index_rp using gin ((c1-c2),c1,c2) global;
--fail ERROR: data type INTEGER has no default operator class for access method "GIN" ???

create unique index CONCURRENTLY rp_index_global18 on hw_global_index_rp (c1) global;
--fail ERROR: not support CONCURRENTLY

create unique index rp_index_global19 on hw_global_index_rp (c1);

select class.relname, class.reltuples, class.parttype from pg_class class, pg_index ind where class.relname = 'rp_index_global1' and ind.indexrelid = class.oid;

drop index rp_index_global1;

drop table hw_global_index_rp;

create table hw_global_index_rp
(
	c1 int,
	c2 int
)
partition by range (c1)
(
	partition hw_global_index_rp_p0 values less than (50),
	partition hw_global_index_rp_p1 values less than (100),
	partition hw_global_index_rp_p2 values less than (150)
);

insert into hw_global_index_rp values(1, 1);
insert into hw_global_index_rp values(1, 1);
insert into hw_global_index_rp values(2, 1);

create unique index rp_index_global21 on hw_global_index_rp (c1) global;
-- fail

delete from hw_global_index_rp where c1 = '1';
create unique index rp_index_global21 on hw_global_index_rp (c1) global;
--succeed

create index rp_index_global22 on hw_global_index_rp(c1 DESC NULLS LAST);
-- succeed

create index rp_index_globa23 on hw_global_index_rp(c1) where c2 > 0;
--fail

alter index rp_index_global22 rename to rp_index_global24;
-- succeed

create index rp_index_globa25 on hw_global_index_rp(c1);
alter index if exists rp_index_global24 unusable;
-- succeed

create index rp_index_globa26 on hw_global_index_rp(c1) tablespace pg_default;
-- succeed

drop table  hw_global_index_rp;

Create table hw_global_index_t1
(
	C1 int,
	C2 int
);

Create unique index t1_unique_index_1 on hw_global_index_t1 (c1) global;
--fail non-partitioned table does not support global global index

drop table hw_global_index_t1;

create table hw_global_index_rp
(
       c1 int,
       c2 int
)
partition by range (c1)
(
       partition hw_global_index_rp_p0 values less than (2000),
       partition hw_global_index_rp_p1 values less than (4000),
       partition hw_global_index_rp_p2 values less than (6000),
       partition hw_global_index_rp_p3 values less than (8000),
       partition hw_global_index_rp_p4 values less than (10000)
);

Create index rp_global_index on hw_global_index_rp (c1) global;

COMMENT ON INDEX rp_global_index IS 'global index comment';

\di+ rp_global_index;
\d rp_global_index;
\d hw_global_index_rp;

select pg_get_indexdef('rp_global_index'::regclass);

COMMENT ON INDEX rp_global_index IS NULL;

\di+ rp_global_index;

drop table hw_global_index_rp;

create table hw_global_index_rp
(
       c1 int,
       name text
)
partition by range (c1)
(
       partition hw_global_index_rp_p0 values less than (2000),
       partition hw_global_index_rp_p1 values less than (4000),
       partition hw_global_index_rp_p2 values less than (6000),
       partition hw_global_index_rp_p3 values less than (8000),
       partition hw_global_index_rp_p4 values less than (10000)
);

Create index rp_global_index on hw_global_index_rp (name) global;

drop table hw_global_index_rp;

create table hw_global_index_rp
(
	c1 int,
	c2 int
)
partition by range (c1)
(
	partition hw_global_index_rp_p0 values less than (2000),
	partition hw_global_index_rp_p1 values less than (4000),
	partition hw_global_index_rp_p2 values less than (6000),
	partition hw_global_index_rp_p3 values less than (8000),
	partition hw_global_index_rp_p4 values less than (10000)
);

insert into hw_global_index_rp values(generate_series(0,9999), 1);

\parallel on
create index rp_index_global1 on hw_global_index_rp (c1) global;
select count(*) from hw_global_index_rp;
\parallel off

drop table hw_global_index_rp;

create table hw_global_index_rp
(
	c1 int,
	c2 int
)
partition by range (c1)
(
	partition hw_global_index_rp_p0 values less than (2000),
	partition hw_global_index_rp_p1 values less than (4000),
	partition hw_global_index_rp_p2 values less than (6000),
	partition hw_global_index_rp_p3 values less than (8000),
	partition hw_global_index_rp_p4 values less than (10000)
);

insert into hw_global_index_rp values(generate_series(0,9999), 1);

create index on hw_global_index_rp(c1);

create index on hw_global_index_rp(c1);

create index on hw_global_index_rp(c1, c2);

create index index_for_create_like on hw_global_index_rp(c2) global;

create table test_gpi_create_like (like hw_global_index_rp including all);

\d test_gpi_create_like;

insert into test_gpi_create_like values(generate_series(0,9999), 1);

explain (costs off) select * from test_gpi_create_like where c1 = 0;
select * from test_gpi_create_like where c1 = 0;

drop table test_gpi_create_like;

drop table if exists gpi_multi_cols;
create table gpi_multi_cols(a0 int,a1 int,a2 int,a3 int,a4 int,a5 int,a6 int,a7 int,a8 int,a9 int,a10 int,a11 int,a12 int,a13 int,a14 int,a15 int,a16 int,a17 int,a18 int,a19 int,a20 int,a21 int,a22 int,a23 int,a24 int,a25 int,a26 int,a27 int,a28 int,a29 int,a30 int,a31 int,a32 int, a33 int) partition by range(a0) (partition p1 values less than (1001), partition p2 values  less than (2001), partition p3 values less than (3001));

-- success
create index gpi_multi_cols_index on gpi_multi_cols(a0,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13,a14,a15,a16,a17,a18,a19,a20,a21,a22,a23,a24,a25,a26,a27,a28,a29,a30) global;
--failed
create index gpi_multi_cols_index on gpi_multi_cols(a0,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13,a14,a15,a16,a17,a18,a19,a20,a21,a22,a23,a24,a25,a26,a27,a28,a29,a30, a31) global;
--success
create index gpi_multi_cols_index_local on gpi_multi_cols(a0,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13,a14,a15,a16,a17,a18,a19,a20,a21,a22,a23,a24,a25,a26,a27,a28,a29,a30, a31) local;

insert into gpi_multi_cols select r,r,r,r,r,r,r,r,r,r,r,r,r,r,r,r,r,r,r,r,r,r,r,r,r,r,r,r,r,r,r,r,r from generate_series(1,3000) as r;

vacuum analyze gpi_multi_cols;
set enable_seqscan = off;
select * from gpi_multi_cols where a1 = 200;
reset enable_seqscan;

--clean data
drop table gpi_multi_cols;
