--sysadmin change eachother passwd
create role ad1 with sysadmin password 'Ttest@123';
create role ad2 with sysadmin password 'Ttest@123';
set role ad1 password 'Ttest@123';
alter role ad2 password 'Ttest@131';
alter role ad2 identified by 'Ttest@1231' replace 'Ttest@123';

--sysadmin change his own passwd
alter role ad1 password 'Ttest@1231';
alter role ad1 identified by 'Ttest@1231' replace 'Ttest@123';

--ordinary user change his own passwd
create user hs password 'Ttest@123';
set role hs password 'Ttest@123';
alter role hs password 'Ttest@1231';
alter role hs identified by 'Ttest@1231' replace 'Ttest@123';

--sysadmin change ordinary user passwd
set role ad1 password 'Ttest@1231';
alter role hs password 'Ttest@12311';

--ordinary user change sysadmin passwd
set role hs password 'Ttest@12311';
alter role ad1 password 'Ttest@12311111';
alter role ad1 identified by 'Ttest@1231111' replace 'Ttest@1231';

--initial account change sysadmin passwd
\c
alter role ad1 password 'Ttest@12311111';
alter role ad1 identified by 'Ttest@123111111' replace 'Ttest@12311111';

create user DB_ACCOUNT_AUTH_OFF_user_041_01 with sysadmin password 'Ttest@123';
create user DB_ACCOUNT_AUTH_OFF_user_041_02 password 'Ttest@123';
set role DB_ACCOUNT_AUTH_OFF_user_041_01 password 'Ttest@123';
alter role DB_ACCOUNT_AUTH_OFF_user_041_02 with sysadmin;
\c
drop user DB_ACCOUNT_AUTH_OFF_user_041_01;
drop user DB_ACCOUNT_AUTH_OFF_user_041_02;


create user DB_ACCOUNT_AUTH_user_047_01 with sysadmin password 'Ttest@123';
create role DB_ACCOUNT_AUTH_user_047_07 with sysadmin password 'Ttest@123';
set role DB_ACCOUNT_AUTH_user_047_01  password 'Ttest@123';
alter role DB_ACCOUNT_AUTH_user_047_07 with nosysadmin;
\c
drop user DB_ACCOUNT_AUTH_user_047_01;
drop user DB_ACCOUNT_AUTH_user_047_07;

--1.system admin can not change each other's password.
--2.system admin can change normal user's password without offer old password.
--3.system admin can change his own password and need offer old password.
--4.inital account can change everyone's password without offer old password expect his own password.
create user tmpuser001 sysadmin password 'test@123';
create user tmpuser002 sysadmin password 'test@123';
set role tmpuser001 password 'test@123';
alter user tmpuser002 identified by 'test@1234' replace 'test@123';
alter user tmpuser002 with sysadmin identified by 'test@1234' replace 'test@123';
reset role;
drop user tmpuser001;
drop user tmpuser002;
