vgram - V-gram indexing and statistics for PostgreSQL
=====================================================

Key idea
--------

`contrib/pg_trgm` indexes substrings whose length is 3.  However, some
3-grams appear to be very frequent, consider `the`, which is a very common
trigram in English.  Additionally, some 2-grams are rare enough to index
them separately, such as `zz`.  The idea of this module is to index `q`-grams
of variable `q`, providing optimal frequency based on the real-life
distribution.

Collecting V-gram statistics
----------------------------

The first thing you need to do is to collect the statistics of frequent
`q`-grams. Based on these statistics, we know that everything longer is
indexable. The `qgram_stat()` SQL aggregate function serves for this purpose.

```sql
qgram_stat(s text, min_q int, max_q int, threshold float8) returns text[]
```

Where `min_q` and `max_q` are the minimal and maximal length of  `q`-gram to
be extracted.  `threshold` is a threshold frequency, everything with
a frequency of this or greater wouldn't be indexed.  The result is an array
of frequent `q`-grams.  This array is to be passed as the `vgram` option
of `vgram_gin_ops` opclass, along with `min_q` and `max_q`.  You can also
check how `vgram` extraction works using the `get_vgrams()` SQL function.

See following example to collect V-gram statistics using `dblp_titles.s`
column, then use these statistics to build an index and accelerate queries
containing LIKE/ILIKE expressions.

```sql
-- Collect V-gram statistics
# SELECT qgram_stat(s, 2, 4, 0.05) AS vgrams FROM titles;
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             vgrams
------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
 {$a,$a$,$an,$and,$ap,$app,$b,$ba,$bas,$c,$co,$com,$con,$d,$de,$di,$e,$f,$fo,$for,$g,$h,$ho,$hom,$i,$in,$in$,$int,$l,$m,$ma,$me,$mo,$mod,$n,$ne,$net,$o,$of,$of$,$on,$on$,$p,$pa,$pag,$pr,$pro,$r,$re,$s,$se,$st,$sy,$sys,$t,$te,$th,$the,$to,$to$,$tr,$u,$us,$v,$w,$wi,$wit,a$,ab,ac,act,ad,ag,age,age$,al,al$,am,an,an$,ana,and,and$,ap,app,ar,as,ase,ased,at,ate,ati,atio,ba,bas,base,bi,bl,c$,ca,cal,cat,cati,ce,ce$,ces,ch,ci,co,com,comp,con,ct,cti,ctio,cu,d$,da,de,del,di,du,e$,ea,ec,ect,ed,ed$,ee,el,el$,em,eme,en,ent,ent$,er,er$,era,eri,es,es$,ess,et,etw,ev,ex,f$,fe,fi,fo,for,for$,form,g$,ge,ge$,gen,gi,gn,gr,gra,h$,ha,he,he$,hi,ho,hom,home,ia,ic,ic$,ica,icat,id,ie,if,ig,il,im,in,in$,ine,ing,ing$,int,inte,io,ion,ion$,ions,ir,is,ist,it,ith,iti,ity,ity$,iv,ive,ive$,iz,k$,l$,la,le,le$,li,lin,ll,lo,lt,lu,ly,m$,ma,man,mat,mati,me,me$,men,ment,mi,mo,mod,mode,mp,ms,ms$,mu,mul,n$,na,nal,nc,nce,nce$,nd,nd$,ne,net,ng,ng$,ni,no,ns,ns$,nt,nt$,nte,o$,ob,oc,od,ode,odel,of,of$,og,ol,om,ome,ome$,omp,on,on$,ons,ons$,op,or,or$,ori,ork,orm,orma,os,ot,ou,pa,pag,page,pe,per,pl,po,pp,pr,pro,pt,qu,r$,ra,ral,rat,rc,re,re$,res,ri,rit,rk,rm,rma,ro,rs,rt,ry,s$,sc,se,sed,sed$,si,sin,sing,so,sp,ss,st,ste,stem,str,su,sy,sys,syst,t$,ta,tat,te,ted,ted$,tem,ter,th,th$,the,the$,ti,tic,tim,tin,ting,tio,tion,tiv,tive,to,to$,tor,tr,tra,tri,ts,ts$,tu,tur,tw,two,ty,ty$,ua,uc,ue,ul,ult,un,ur,ure,us,usi,usin,ut,va,ve,ve$,ver,vi,wi,wit,with,wo,wor,work,y$,ys,yst,yste}
(1 row)

-- Save it as psql variable
# \gset

-- Check V-gram extraction
# SELECT get_vgrams('indexing', 2, 4, :'vgrams');
    get_vgrams
------------------
 {ind,nde,dex,xi}
(1 row)

-- Build V-gram index
# CREATE INDEX titles_s_idx ON titles USING gin (s vgram_gin_ops (minq=2, maxq=4, vgrams=:'vgrams'));
CREATE INDEX

# EXPLAIN SELECT * FROM titles WHERE s LIKE '%indexing%';
                                 QUERY PLAN
----------------------------------------------------------------------------
 Bitmap Heap Scan on titles  (cost=38.88..42.89 rows=1 width=48)
   Recheck Cond: (s ~~ '%indexing%'::text)
   ->  Bitmap Index Scan on titles_s_idx  (cost=0.00..38.88 rows=1 width=0)
         Index Cond: (s ~~ '%indexing%'::text)
(4 rows)

# SELECT * FROM titles WHERE s LIKE '%indexing%';
                                           s
----------------------------------------------------------------------------------------
 Hybrid term indexing for different IR models.
 Efficient speech indexing and search for embedded devices using uniterms.
 The EURATOM automatic indexing project.
 Spectral cross-correlation features for audio indexing of broadcast news and meetings.
(4 rows)
```

Advanced statistics
-------------------

You also may use `vgram_text` datatype instead of simple `text` and take
advantage of statistics for frequent individual characters, 2-grams, and
3-grams.  These statistics provide selectivity estimation for LIKE/ILIKE
using Markov's model.  LIKE/ILIKE operators are implemented directly for
`vgram_text` while the rest of string operations are available using implicit
cast to `text`.  Note that you must use `vgram_gin_ops2` opclass for
`vgram_text` columns.

```sql
# CREATE TABLE titles (s text);
CREATE TABLE

# \copy titles from 'data/titles.data'
COPY 10000

# ANALYZE titles;
ANALYZE

# EXPLAIN ANALYZE SELECT * FROM titles WHERE s LIKE '%the%';
                                                 QUERY PLAN
------------------------------------------------------------------------------------------------------------
 Seq Scan on titles  (cost=0.00..225.00 rows=1030 width=48) (actual time=0.018..2.075 rows=1103.00 loops=1)
   Filter: (s ~~ '%the%'::text)
   Rows Removed by Filter: 8897
   Buffers: shared hit=100
 Planning Time: 0.094 ms
 Execution Time: 2.137 ms
(6 rows)

Time: 2.783 ms
# EXPLAIN ANALYZE SELECT * FROM titles WHERE s LIKE '%zz%';
                                               QUERY PLAN
---------------------------------------------------------------------------------------------------------
 Seq Scan on titles  (cost=0.00..225.00 rows=64 width=48) (actual time=0.043..1.556 rows=105.00 loops=1)
   Filter: (s ~~ '%zz%'::text)
   Rows Removed by Filter: 9895
   Buffers: shared hit=100
 Planning Time: 0.145 ms
 Execution Time: 1.590 ms
(6 rows)
```

Author
------

 * Alexander Korotkov <aekorotkov@gmail.com>, OrioleDB

Availability
------------

vgram is realized as an extension and not available in default PostgreSQL
installation. It is available from
[github](https://github.com/akorotkov/vgram)
under the same license as
[PostgreSQL](https://www.postgresql.org/about/licence/)
and supports PostgreSQL 9.4+.
